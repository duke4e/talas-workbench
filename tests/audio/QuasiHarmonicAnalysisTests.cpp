#include "talas/audio/PartialTrackAudioAnalysis.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/audio/QuasiHarmonicAnalysis.h"
#include "talas/audio/ResourceLoader.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

// The Spectral Analysis tool's way from a
// straight spectrum to a harmonic one.
namespace
{
using talas::audio::FundamentalPoint;
using talas::audio::FundamentalTraceBias;
using talas::project::PartialTrackAsset;
using talas::project::PartialTrackFrame;
using talas::project::PartialTrackPoint;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

// A frame of peaks at the frequencies and amplitudes given.
PartialTrackFrame frame(std::uint64_t sourceFrame, const std::vector<std::pair<double, double>>& peaks)
{
    PartialTrackFrame out{1, sourceFrame, {}};
    std::uint64_t id = 1;
    for (const auto& [hz, amplitude] : peaks)
        out.points.push_back({id++, amplitude, hz, 10.0});
    return out;
}

// A harmonic tone's peaks: its fundamental and harmonics at 1/k.
std::vector<std::pair<double, double>> harmonics(double fundamental, int count)
{
    std::vector<std::pair<double, double>> peaks;
    for (int k = 1; k <= count; ++k)
        peaks.push_back({fundamental * k, 1.0 / k});
    return peaks;
}

PartialTrackAsset asset(std::vector<PartialTrackFrame> frames, std::uint64_t length = 48000)
{
    return {48000.0, length, std::move(frames)};
}

// The sample loops with a silence after it.
void testTheSampleLoopsWithASilence()
{
    const std::vector<double> samples(10, 0.5);
    const auto looped = talas::audio::sampleWithSilence(samples, 10.0, 1.0);
    require(looped.size() == 20 && looped[9] == 0.5 && looped[10] == 0.0 && looped[19] == 0.0,
        "a second of silence follows the sample");
    require(talas::audio::sampleWithSilence(samples, 10.0, 0.0).size() == 10, "no silence typed leaves the sample alone");
}

// A harmonic frame is pitched, noise is not, and the fader
// takes the most pitched frames out first.
void testPitchedFramesAndTheFader()
{
    const auto straight = asset({
        frame(0, harmonics(200.0, 6)),
        frame(480, {{1234.0, 0.4}, {1789.0, 0.4}, {2333.0, 0.4}, {2911.0, 0.4}, {3507.0, 0.4}, {4127.0, 0.4}}),
        frame(960, {}),
    });
    const auto pitches = talas::audio::framePitches(straight, 100.0);
    require(pitches.size() == 3, "each frame is judged");
    require(pitches[0].pitchedness > 0.99 && near(pitches[0].fundamentalHz, 200.0, 1.0e-6),
        "a harmonic frame is pitched at its fundamental: " + std::to_string(pitches[0].fundamentalHz));
    require(pitches[1].pitchedness < 0.8, "noise is less pitched: " + std::to_string(pitches[1].pitchedness));
    require(pitches[2].pitchedness == 0.0 && pitches[2].fundamentalHz == 0.0, "a silent frame has no pitch");
    require(!talas::audio::framePitched(pitches[0], 0.0), "the fader at 0 leaves every frame unpitched");
    require(talas::audio::framePitched(pitches[0], 0.15) && !talas::audio::framePitched(pitches[1], 0.15),
        "raising the fader takes the vowels out first, the consonants left");
    require(talas::audio::framePitched(pitches[1], 1.0) && !talas::audio::framePitched(pitches[2], 1.0),
        "the fader at its top takes out all but silence");
}

// The unpitched parts heard alone.
void testTheUnpitchedPartsHeardAlone()
{
    const auto straight = asset({frame(0, {}), frame(100, harmonics(200.0, 4)), frame(200, harmonics(200.0, 4)),
        frame(300, {})}, 400);
    const auto pitches = talas::audio::framePitches(straight, 100.0);
    const std::vector<double> samples(400, 1.0);
    const auto heard = talas::audio::unpitchedOnly(samples, straight, pitches, 0.5);
    require(heard.size() == 400 && heard[50] == 1.0 && heard[99] == 1.0, "the unpitched frames before are heard");
    require(heard[200] == 0.0 && heard[299] == 0.0, "the pitched frames are silenced");
    require(heard[124] > 0.0 && heard[124] < 1.0, "a pitched frame fades out over a millisecond");
    require(heard[399] == 1.0, "the unpitched frame after is heard again");
    require(talas::audio::unpitchedOnly(samples, straight, pitches, 0.0) == samples, "the fader at 0 silences nothing");
}

// The initial guess at the fundamental, on the harmonic named.
void testTheTraceFollowsTheFundamental()
{
    std::vector<PartialTrackFrame> frames;
    for (int index = 0; index < 10; ++index)
        frames.push_back(frame(static_cast<std::uint64_t>(index) * 480, harmonics(200.0 + 2.0 * index, 6)));
    const auto straight = asset(frames);
    const auto pitches = talas::audio::framePitches(straight, 100.0);
    const auto trace = talas::audio::traceFundamental(straight, pitches, 0.5, 100.0);
    require(trace.size() == 2 && trace.front() == FundamentalPoint{0.0, 200.0} && near(trace.back().seconds, 0.09)
            && near(trace.back().hz, 218.0),
        "the points a straight glide gives thin to its ends: " + std::to_string(trace.size()));
    require(near(talas::audio::fundamentalAt(trace, 0.045), 209.0, 1.0e-9), "the trace is linear between its points");
    require(talas::audio::fundamentalAt(trace, -1.0) == 200.0 && talas::audio::fundamentalAt(trace, 5.0) == 218.0
            && talas::audio::fundamentalAt({}, 1.0) == 0.0,
        "the trace holds beyond its ends");

    const auto second = talas::audio::traceFundamental(straight, pitches, 0.5, 100.0, 0.0, FundamentalTraceBias::Guess, 2);
    require(near(second.front().hz, 400.0) && near(talas::audio::fundamentalAt(second, 0.0, 2), 200.0),
        "a trace on the second harmonic follows it at twice the fundamental");
    require(talas::audio::traceFundamental(straight, pitches, 0.0, 100.0).empty(), "with no pitched frames, no trace");

    std::vector<PartialTrackFrame> broken = frames;
    broken[5] = frame(broken[5].sourceFrame, {});
    const auto gapped = asset(broken);
    const auto runs = talas::audio::traceFundamental(gapped, talas::audio::framePitches(gapped, 100.0), 0.5, 100.0);
    require(runs.size() == 4 && near(runs[1].hz, 208.0) && near(runs[2].hz, 212.0),
        "an unpitched frame ends a run of the trace, each run keeping its ends");
}

// Auto trace lower for segments that jumped up to the next
// partial, higher for those that jumped down.
void testAutoTraceLowerAndHigher()
{
    const auto weak = asset({frame(0, {{200.0, 0.05}, {400.0, 1.0}, {600.0, 0.1}, {800.0, 0.8}, {1000.0, 0.05},
        {1200.0, 0.6}})});
    const auto pitches = talas::audio::framePitches(weak, 100.0);
    const auto guessed = talas::audio::traceFundamental(weak, pitches, 0.5, 100.0);
    require(guessed.size() == 1 && near(guessed.front().hz, 400.0),
        "a weak fundamental lets the guess jump up to the next partial");
    const auto lower = talas::audio::traceFundamental(weak, pitches, 0.5, 100.0, 0.0, FundamentalTraceBias::Lower);
    require(lower.size() == 1 && near(lower.front().hz, 200.0), "Auto trace lower brings it back to the fundamental");

    const auto clean = asset({frame(0, harmonics(200.0, 6))});
    const auto higher = talas::audio::traceFundamental(
        clean, talas::audio::framePitches(clean, 100.0), 0.5, 100.0, 0.0, FundamentalTraceBias::Higher);
    require(higher.size() == 1 && near(higher.front().hz, 400.0), "Auto trace higher moves it up a partial");
}

// Hand trace's points added with Shift+Click, moved and
// deleted.
void testEditingTheTrace()
{
    std::vector<FundamentalPoint> trace;
    auto added = talas::audio::traceWithPoint(trace, {1.0, 220.0});
    added = talas::audio::traceWithPoint(*added, {0.5, 200.0});
    added = talas::audio::traceWithPoint(*added, {2.0, 240.0});
    require(added && *added == std::vector<FundamentalPoint>{{0.5, 200.0}, {1.0, 220.0}, {2.0, 240.0}},
        "points added by hand keep their time order");
    require(!talas::audio::traceWithPoint(*added, {1.0, 300.0}) && !talas::audio::traceWithPoint(*added, {3.0, 0.0})
            && !talas::audio::traceWithPoint(*added, {-1.0, 200.0}),
        "a point on another's time, or of no frequency, is refused");
    const auto moved = talas::audio::traceWithPointMoved(*added, 0, {1.5, 210.0});
    require(moved && *moved == std::vector<FundamentalPoint>{{1.0, 220.0}, {1.5, 210.0}, {2.0, 240.0}},
        "a point dragged past another takes its place in time");
    require(!talas::audio::traceWithPointMoved(*added, 0, {2.0, 210.0}) && !talas::audio::traceWithPointMoved(*added, 5, {4.0, 1.0}),
        "a point cannot be dragged onto another's time");
    const auto deleted = talas::audio::traceWithoutPoint(*moved, 1);
    require(deleted && *deleted == std::vector<FundamentalPoint>{{1.0, 220.0}, {2.0, 240.0}} && !talas::audio::traceWithoutPoint(*deleted, 2),
        "Backspace deletes the selected point");
}

// Fundamental Only: a sine at the traced fundamental.
void testTheFundamentalAsASine()
{
    const std::vector<FundamentalPoint> trace{{0.0, 200.0}};
    const auto sine = talas::audio::fundamentalSine(trace, 2, 1000.0, 20);
    require(sine.size() == 20 && sine[0] == 0.0 && near(sine[10], 0.0, 1.0e-9)
            && near(sine[2], 0.25 * std::sin(2.0 * std::numbers::pi * 0.2), 1.0e-9),
        "the sine plays the fundamental, the trace's frequency over the traced harmonic");
    require(talas::audio::fundamentalSine({}, 1, 1000.0, 4) == std::vector<double>(4, 0.0), "no trace, no sine");
}

// All Harmonics: each track a harmonic of the trace.
void testTheHarmonicSpectrum()
{
    const auto straight = asset({frame(0, {{200.0, 1.0}, {405.0, 0.5}, {590.0, 0.3}, {1234.0, 0.2}}), frame(480, {})});
    const std::vector<FundamentalPoint> trace{{0.0, 200.0}};
    const auto harmonic = talas::audio::harmonicSpectrum(straight, trace, 1, 4);
    require(harmonic.frames.size() == 2 && harmonic.frames[0].points.size() == 4, "the harmonics up to the limit");
    const auto& points = harmonic.frames[0].points;
    require(points[0] == PartialTrackPoint{1, 1.0, 200.0, 10.0} && points[1] == PartialTrackPoint{2, 0.5, 405.0, 10.0}
            && points[2] == PartialTrackPoint{3, 0.3, 590.0, 10.0} && points[3] == PartialTrackPoint{4, 0.0, 800.0, 0.0},
        "each harmonic takes the peak nearest k times the fundamental, or is silent there");
    require(harmonic.frames[1].points.size() == 4 && harmonic.frames[1].points[2].amplitude == 0.0,
        "a frame with no peaks keeps its silent harmonics");
    require(talas::project::encodePartialTrackAsset(harmonic).ok(), "the harmonic spectrum is a valid spectrum file");
    const std::vector<FundamentalPoint> high{{0.0, 5000.0}};
    const auto low = talas::audio::harmonicSpectrum(asset({frame(0, harmonics(5000.0, 2))}), high);
    require(low.frames[0].points.size() == 4, "harmonics stop at half the sample rate");
}

// The harmonic spectrum as a SumOfSines analysis table.
void testTheSumOfSinesTable()
{
    PartialTrackAsset harmonic{48000.0, 1440, {
        {1, 0, {{1, 0.5, 200.0, 0.0}, {2, 0.25, 400.0, 0.0}}},
        {1, 480, {{1, 0.4, 200.0, 0.0}}},
        {1, 960, {{1, 0.3, 200.0, 0.0}, {2, 0.2, 400.0, 0.0}, {3, 0.1, 600.0, 0.0}}},
    }};
    const auto table = talas::audio::harmonicAmplitudeTable(harmonic);
    require(table.frames == 3 && table.partials == 3
            && table.amplitudes == std::vector<double>{0.5, 0.25, 0.0, 0.4, 0.0, 0.0, 0.3, 0.2, 0.1},
        "each frame's harmonics in order, those it lacks silent");
    const auto fewer = talas::audio::harmonicAmplitudeTable(harmonic, 2, 2);
    require(fewer.frames == 2 && fewer.partials == 2 && fewer.amplitudes == std::vector<double>{0.5, 0.25, 0.3, 0.2},
        "frames evenly spaced through the spectrum and harmonics up to the limit");
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-harmonic-table-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    std::string error;
    require(talas::audio::writeHarmonicAmplitudeTable(folder / "voice h.txt", table, error), "the table is written: " + error);
    const auto loaded = talas::audio::loadTableResource(folder / "voice h.txt");
    require(loaded.ok() && loaded.samples == table.amplitudes, "a table resource reads it back: " + loaded.error);
    require(!talas::audio::writeHarmonicAmplitudeTable(folder / "empty.txt", {}, error), "an empty table is refused");
    std::filesystem::remove_all(folder);
}

// The pitch hints, and middle C for speech.
void testThePitchHints()
{
    const std::vector<FundamentalPoint> trace{{0.0, 200.0}, {1.0, 300.0}, {2.0, 200.0}};
    const auto summary = talas::audio::fundamentalSummary(trace);
    require(summary && summary->startHz == 200.0 && summary->endHz == 200.0 && near(summary->averageHz, 250.0),
        "the starting, ending and average frequencies");
    require(near(talas::audio::fundamentalSummary(trace, 2)->averageHz, 125.0), "on the traced harmonic over its number");
    require(!talas::audio::fundamentalSummary({}), "no trace, no hints");
    require(near(talas::audio::noteNumberOfFrequency(440.0), 69.0) && near(talas::audio::noteNumberOfFrequency(261.6256), 60.0, 1.0e-4)
            && talas::audio::defaultHarmonicAnalysisNoteNumber == 60.0,
        "pitches as note numbers, middle C 60");
}

// The steps on a sound: a pitched half and a noisy half.
void testAVoiceAndABreath()
{
    constexpr double sampleRate = 48000.0;
    std::vector<double> audio(24000, 0.0);
    std::uint32_t state = 12345;
    for (std::size_t frame = 0; frame < audio.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / sampleRate;
        if (frame < 12000)
        {
            for (int k = 1; k <= 5; ++k)
                audio[frame] += 0.3 / k * std::sin(2.0 * std::numbers::pi * 220.0 * k * t);
        }
        else
        {
            state = state * 1664525U + 1013904223U;
            audio[frame] = 0.3 * (static_cast<double>(state >> 8) / static_cast<double>(1U << 24) - 0.5);
        }
    }
    talas::audio::PartialTrackAudioAnalysisSettings settings;
    settings.lowestFrequencyHz = 87.31;
    settings.kind = talas::audio::PartialTrackAnalysisKind::Straight;
    const auto straight = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, settings);
    require(straight.ok(), "the straight analysis runs");
    const auto pitches = talas::audio::framePitches(straight.asset, settings.lowestFrequencyHz);
    double voiced = 0.0;
    double breath = 0.0;
    std::size_t voicedFrames = 0;
    std::size_t breathFrames = 0;
    for (std::size_t index = 0; index < pitches.size(); ++index)
    {
        const auto at = straight.asset.frames[index].sourceFrame;
        if (at > 2000 && at < 10000)
        {
            voiced += pitches[index].pitchedness;
            ++voicedFrames;
        }
        else if (at > 14000 && at < 22000)
        {
            breath += pitches[index].pitchedness;
            ++breathFrames;
        }
    }
    require(voicedFrames > 0 && breathFrames > 0 && voiced / voicedFrames > breath / breathFrames + 0.2,
        "the tone is more pitched than the noise: " + std::to_string(voiced / voicedFrames) + " against "
            + std::to_string(breath / breathFrames));
    const auto trace = talas::audio::traceFundamental(straight.asset, pitches, 0.2, settings.lowestFrequencyHz);
    require(!trace.empty() && std::abs(talas::audio::fundamentalAt(trace, 0.1) - 220.0) < 220.0 * 0.03,
        "the trace finds the tone's fundamental: " + std::to_string(talas::audio::fundamentalAt(trace, 0.1)));
    const auto harmonic = talas::audio::harmonicSpectrum(straight.asset, trace);
    const auto rendered = talas::audio::renderPartialTrackAssetOffline(harmonic);
    require(talas::project::encodePartialTrackAsset(harmonic).ok() && rendered.ok() && rendered.samples.size() == audio.size(),
        "All Harmonics resynthesizes the harmonic spectrum");
}
}

int main()
{
    try
    {
        testTheSampleLoopsWithASilence();
        testPitchedFramesAndTheFader();
        testTheUnpitchedPartsHeardAlone();
        testTheTraceFollowsTheFundamental();
        testAutoTraceLowerAndHigher();
        testEditingTheTrace();
        testTheFundamentalAsASine();
        testTheHarmonicSpectrum();
        testTheSumOfSinesTable();
        testThePitchHints();
        testAVoiceAndABreath();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
