#include "talas/app/PartialTrackSpectrumEditing.h"
#include "talas/app/SpectrumSynchronization.h"
#include "talas/audio/PartialTrackAudioAnalysis.h"
#include "talas/audio/QuasiHarmonicAnalysis.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/audio/SpectrumTables.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/runtime/ProjectRenderSettings.h"
#include "talas/runtime/Runtime.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <utility>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// A spectrum file played by SpectrumInRAM:
// its tracks as frames of amplitude, frequency and bandwidth triples, read
// through the TimeIndex into an OscillatorBank.
namespace
{
constexpr double sampleRate = 48000.0;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1.0e-9)
{
    return std::abs(a - b) <= tolerance;
}

const talas::core::NodeRegistry& registry()
{
    static const auto builtins = talas::core::makeBuiltinNodeRegistry();
    return builtins;
}

talas::project::ProjectParameterExpression expression(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression resource(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

// The amplitude of a frequency in samples.
double toneLevel(std::span<const double> samples, double frequency)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency * static_cast<double>(frame) / sampleRate;
        real += samples[frame] * std::cos(phase);
        imaginary += samples[frame] * std::sin(phase);
    }
    return 2.0 * std::hypot(real, imaginary) / static_cast<double>(samples.size());
}

talas::project::PartialTrackAsset spectrum(std::vector<talas::project::PartialTrackFrame> frames)
{
    return {sampleRate, 1440, std::move(frames)};
}

// The table through a SpectrumInRAM into an OscillatorBank, the spectrum
// out or its amplitude and frequency pairs.
std::vector<double> played(const talas::audio::SpectrumInRamTable& table, bool pairs, std::size_t frames)
{
    talas::project::ProjectDocument document;
    const auto count = std::to_string(table.partials);
    document.graph.nodes = {
        {"ram", "talas.spectrumInRam", {expression("frequency", "0 hz"), expression("level", "1"),
            expression("timeIndex", "-1"), resource("analysis", "Spectrum/Voice"),
            expression("analysisFrames", std::to_string(table.frames)), expression("analysisFrequency", "1 hz"),
            expression("nbrPartials", count), expression("firstPartial", "1")}},
        {"bank", "talas.oscillatorBank", {expression("nbrOscillators", count), expression("bankSize", count)}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"bank", "audioOut", "out", "audioIn"}};
    if (pairs)
    {
        document.graph.connections.push_back({"ram", "amplitudeOut", "bank", "amplitudeIn"});
        document.graph.connections.push_back({"ram", "frequencyOut", "bank", "frequencyIn"});
    }
    else
    {
        document.graph.connections.push_back({"ram", "spectrumOut", "bank", "spectrum"});
    }
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = {{"Spectrum/Voice", table.triples}};
    auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "the SpectrumInRAM should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    auto rendered = talas::runtime::renderOffline(*compiled.graph, frames);
    require(rendered.ok(), "the SpectrumInRAM should render");
    return rendered.mono;
}

void testEachTrackIsASlotOfTriples()
{
    const auto voice = spectrum({
        {1, 0, {{1, 0.5, 440.0, 10.0}, {3, 0.25, 880.0, 20.0}}},
        {1, 480, {{1, 0.4, 442.0, 11.0}}},
        {1, 960, {{1, 0.3, 444.0, 12.0}, {3, 0.2, 886.0, 22.0}}},
    });
    const auto table = talas::audio::spectrumInRamTable(voice);
    require(table.frames == 3 && table.partials == 2 && near(table.durationSeconds, 0.03),
        "a frame for each of the spectrum's, a slot for each of its tracks");
    require(table.triples == std::vector<double>{0.5, 440.0, 10.0, 0.25, 880.0, 20.0, 0.4, 442.0, 11.0, 0.0, 880.0, 0.0,
                0.3, 444.0, 12.0, 0.2, 886.0, 22.0},
        "each slot holds its track's amplitude, frequency and bandwidth, silent at its last frequency where it is missing");
    const auto fewer = talas::audio::spectrumInRamTable(voice, 2);
    require(fewer.frames == 2 && fewer.triples[6] == 0.3, "frames are evenly spaced through the spectrum");
    const auto bounded = talas::audio::spectrumInRamTable(voice, 4096, 3);
    require(bounded.frames == 1 && bounded.partials == 2, "the frames fit the triples allowed");
    require(talas::audio::spectrumInRamTable(spectrum({})).frames == 0, "an empty spectrum has no table");
}

void testTheTableIsWrittenAsATableResource()
{
    const auto table = talas::audio::spectrumInRamTable(spectrum({{1, 0, {{1, 0.5, 440.0, 10.0}, {2, 0.25, 880.0, 20.0}}}}));
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-spectrum-table-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    std::string error;
    require(talas::audio::writeSpectrumInRamTable(folder / "voice table.txt", table, error), "the table is written: " + error);
    const auto loaded = talas::audio::loadTableResource(folder / "voice table.txt");
    require(loaded.ok() && loaded.samples == table.triples, "a table resource reads it back: " + loaded.error);
    require(!talas::audio::writeSpectrumInRamTable(folder / "empty.txt", {}, error), "an empty table is refused");
    std::filesystem::remove_all(folder);
}

// SpectrumInRAM's spectrum, or its amplitude and frequency
// pairs, played by an OscillatorBank at the tracks' frequencies.
void testSpectrumInRamPlaysTheSpectrum()
{
    const auto table = talas::audio::spectrumInRamTable(spectrum({{1, 0, {{1, 0.5, 440.0, 0.0}, {2, 0.25, 880.0, 0.0}}}}));
    for (const auto pairs : {false, true})
    {
        const auto audio = played(table, pairs, 9600);
        const std::span settled{audio.data() + 4800, 4800};
        const auto low = toneLevel(settled, 440.0);
        const auto high = toneLevel(settled, 880.0);
        require(low > 0.01 && std::abs(low / high - 2.0) < 0.05 && toneLevel(settled, 660.0) < 0.1 * high,
            std::string{"the tracks play at their frequencies and levels through "} + (pairs ? "the pairs" : "the spectrum")
                + ": " + std::to_string(low) + " " + std::to_string(high));
    }
}

// A graph rendered at a block size, its left channel.
std::vector<double> renderedLeft(talas::project::ProjectDocument document, talas::runtime::RenderSettings settings, std::size_t frames)
{
    auto compiled = talas::runtime::compileProject(document, registry(), std::move(settings));
    require(compiled.ok(), "the graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    auto output = talas::runtime::renderOffline(*compiled.graph, frames);
    require(output.ok(), "the graph should render: "
            + (output.diagnostics.empty() ? std::string{} : output.diagnostics.front().message));
    return output.mono;
}

// Behind the scenes a SumOfSines is a SpectrumInRAM
// controlling an OscillatorBank. A SumOfSines on a table of three
// harmonics' amplitudes, frame by frame, at 220 hz, and a SpectrumInRAM on
// the same amplitudes at 220, 440 and 660 hz into an OscillatorBank, give
// the same samples: at fixed points of the analysis, between its frames,
// and with their TimeIndex swept through it by the same FunctionGenerator.
void testTheSumOfSinesIsASpectrumInRamControllingAnOscillatorBank()
{
    const std::vector<double> amplitudes{0.5, 0.25, 0.125, 0.2, 0.4, 0.1, 0.05, 0.1, 0.3};
    std::vector<double> triples;
    for (std::size_t index = 0; index < amplitudes.size(); ++index)
    {
        triples.push_back(amplitudes[index]);
        triples.push_back(220.0 * static_cast<double>(index % 3 + 1));
        triples.push_back(0.0);
    }
    const auto sumOfSines = [](std::string timeIndex) {
        return talas::project::ProjectNode{"sines", "talas.sumOfSines",
            {resource("analysis0", "Tables/Harmonics"), expression("analysisFrequency0", "220 hz"),
                expression("analysisFrames0", "3"), expression("frequency0", "220 hz"), expression("frequency1", "220 hz"),
                expression("nbrPartials", "3"), expression("bankSize", "3"), expression("onDuration", "1 s"),
                expression("envelope", "1"), expression("timeIndex", std::move(timeIndex)),
                [] {
                    talas::project::ProjectParameterExpression controlled;
                    controlled.parameterId = "ctrlTime";
                    controlled.boolean = true;
                    return controlled;
                }()}};
    };
    const auto spectrumInRam = [](std::string timeIndex) {
        return talas::project::ProjectNode{"ram", "talas.spectrumInRam",
            {expression("frequency", "0 hz"), expression("level", "1"), expression("timeIndex", std::move(timeIndex)),
                resource("analysis", "Tables/Triples"), expression("analysisFrames", "3"),
                expression("analysisFrequency", "1 hz"), expression("nbrPartials", "3"), expression("firstPartial", "1")}};
    };
    const auto ramp = [] {
        return talas::project::ProjectNode{"ramp", "talas.functionGenerator",
            {expression("trigger", "1"), expression("onDuration", "0.2 s"), resource("wavetable", "FullRamp")}};
    };
    const auto pair = [&](std::string timeIndex, bool swept, std::size_t blockSize, std::size_t frames = 4800) {
        talas::runtime::RenderSettings settings{sampleRate, blockSize};
        settings.tableResources = {{"Tables/Harmonics", amplitudes}, {"Tables/Triples", triples}};
        talas::project::ProjectDocument sines;
        sines.graph.nodes = {sumOfSines(timeIndex), {"out", "talas.audioOutput", {}}};
        sines.graph.connections = {{"sines", "audioOut", "out", "audioIn"}};
        talas::project::ProjectDocument bank;
        bank.graph.nodes = {spectrumInRam(timeIndex),
            {"bank", "talas.oscillatorBank", {expression("nbrOscillators", "3"), expression("bankSize", "3")}},
            {"out", "talas.audioOutput", {}}};
        bank.graph.connections = {{"ram", "spectrumOut", "bank", "spectrum"}, {"bank", "audioOut", "out", "audioIn"}};
        if (swept)
        {
            sines.graph.nodes.push_back(ramp());
            sines.graph.connections.push_back({"ramp", "value", "sines", "timeIndex"});
            bank.graph.nodes.push_back(ramp());
            bank.graph.connections.push_back({"ramp", "value", "ram", "timeIndex"});
        }
        return std::pair{renderedLeft(sines, settings, frames), renderedLeft(bank, settings, frames)};
    };
    const auto same = [](const std::vector<double>& left, const std::vector<double>& right, const std::string& what) {
        require(left.size() == right.size(), what + ": the two should be as long");
        double loudest = 0.0;
        for (std::size_t frame = 0; frame < left.size(); ++frame)
        {
            loudest = std::max(loudest, std::abs(left[frame]));
            // Within a millionth, -120 dB: the two round differently.
            require(std::abs(left[frame] - right[frame]) < 1.0e-6,
                what + ": the SumOfSines and the SpectrumInRAM's OscillatorBank should play the same sample at frame "
                    + std::to_string(frame) + ": " + std::to_string(left[frame]) + " " + std::to_string(right[frame])
                    + ", " + std::to_string((left[frame] - right[frame]) * 1.0e9) + "e-9 apart");
        }
        require(loudest > 0.1, what + ": the two should sound");
    };
    for (const auto* timeIndex : {"-1", "-0.3", "0", "0.5", "1"})
    {
        const auto [sines, bank] = pair(timeIndex, false, 64);
        same(sines, bank, std::string{"at TimeIndex "} + timeIndex);
    }
    // One sample a block, so that the OscillatorBank takes every frame the
    // SpectrumInRAM gives, as the SumOfSines does.
    const auto [sweptSines, sweptBank] = pair("0", true, 1, 9600);
    same(sweptSines, sweptBank, "with the TimeIndex swept");
    // The first 20 ms of the 0.2 s sweep, and 20 ms near its end.
    const std::span early{sweptSines.data(), 960};
    const std::span late{sweptSines.data() + 8160, 960};
    require(toneLevel(early, 220.0) > 1.5 * toneLevel(early, 440.0) && toneLevel(late, 660.0) > 1.5 * toneLevel(late, 220.0),
        "the sweep should move through the analysis, the fundamental first and the third harmonic last");
}

// A steady spectrum of two partials, ten frames of 10 ms.
talas::project::PartialTrackAsset steady(double fundamental)
{
    std::vector<talas::project::PartialTrackFrame> frames;
    for (std::uint64_t index = 0; index < 10; ++index)
        frames.push_back({1, index * 480, {{1, 0.5, fundamental, 0.0}, {2, 0.25, fundamental * 2.0, 0.0}}});
    return {sampleRate, 4800, std::move(frames)};
}

std::pair<std::vector<double>, std::vector<double>> playedWithValues(talas::app::AnalysisExampleSound example,
    const std::vector<talas::runtime::TableResource>& tables, const std::vector<std::pair<std::string, double>>& values,
    std::size_t frames, const std::vector<talas::runtime::EventValueBinding>& keys = {});

// An example Sound played, its tables named by the resources it takes and
// !Morph at the value given; both channels.
std::pair<std::vector<double>, std::vector<double>> playedExample(
    talas::app::AnalysisExampleSound example, const std::vector<talas::runtime::TableResource>& tables, double morph,
    std::size_t frames = 4800)
{
    return playedWithValues(std::move(example), tables, {{"Morph", morph}}, frames);
}

// An example Sound played, its controls at the values it brings, its tables
// named by the resources it takes, its Event Values at the values given and
// the keyboard's at `keys`; both channels.
std::pair<std::vector<double>, std::vector<double>> playedWithValues(talas::app::AnalysisExampleSound example,
    const std::vector<talas::runtime::TableResource>& tables, const std::vector<std::pair<std::string, double>>& values,
    std::size_t frames, const std::vector<talas::runtime::EventValueBinding>& keys)
{
    for (auto& node : example.definition.graph.nodes)
    {
        for (const auto& bound : example.resources)
        {
            if (bound.nodeId == node.instanceId)
                node.parameters.push_back(resource(bound.parameterId, talas::project::pathToUtf8(bound.file)));
        }
    }
    const auto definitionId = example.definition.definitionId;
    const auto stereo = example.definition.outputs.size() == 2;
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    require(editor.addSoundDefinition(std::move(example.definition)).ok(), "the example should be a valid Sound");
    // The example's controls start where it brings them, as the Workbench
    // sets them with its Sound (the Sound already made them, at 0).
    for (const auto& control : example.controls)
    {
        const auto ranged = editor.setVirtualControlRange(control.eventName, control.minimum, control.maximum);
        const auto set = editor.setVirtualControlValue(control.eventName, control.value);
        require(ranged.ok() && set.ok(), "the example's controls should start at their values: " + ranged.message + set.message);
    }
    require(editor.addSoundInstance("example", definitionId, {0, 0}).ok()
            && editor.addNode({"out", "talas.audioOutput", {}, {}, {}}, {200, 0}).ok()
            && editor.connect({"example", "audioOut", "out", "audioIn"}).ok()
            && (!stereo || editor.connect({"example", "out2", "out", "rightIn"}).ok()),
        "the example should play");
    auto document = editor.document();
    (void) talas::project::synchronizeProjectEventValueControls(document, registry());
    talas::runtime::RenderSettings settings{sampleRate, 64};
    settings.tableResources = tables;
    auto configured = talas::runtime::prepareProjectRenderSettings(document, registry(), std::move(settings));
    require(configured.ok(), "the example's render settings: " + configured.error);
    for (auto& value : configured.settings->eventValues)
    {
        for (const auto& [name, given] : values)
        {
            if (value.name == name)
                value.value = given;
        }
    }
    // A MIDI keyboard's values, which no control on the surface holds.
    for (const auto& key : keys)
        configured.settings->eventValues.push_back(key);
    auto compiled = talas::runtime::compileProject(document, registry(), std::move(*configured.settings));
    require(compiled.ok(), "the example should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    auto rendered = talas::runtime::renderOffline(*compiled.graph, frames);
    require(rendered.ok() && !rendered.channels.empty(), "the example should render");
    return {rendered.channels.front(), rendered.channels.size() > 1 ? rendered.channels[1] : rendered.channels.front()};
}

// The mix plays the guide left and the warped
// right; the morph is the guide at Morph 0 and the warped at 1.
void testTheMixAndTheMorph()
{
    const auto guide = steady(440.0);
    const auto warped = steady(660.0);
    const auto guideTable = talas::audio::spectrumInRamTable(guide);
    const auto warpedTable = talas::audio::spectrumInRamTable(warped);
    const talas::app::SynchronizedSpectrum guided{"Tables/Guide", guideTable.frames, guideTable.partials,
        talas::app::spectrumTimePoints(guide, talas::app::autoSpectrumMarkers(guide))};
    const talas::app::SynchronizedSpectrum warping{"Tables/Warped", warpedTable.frames, warpedTable.partials,
        talas::app::spectrumTimePoints(warped, talas::app::autoSpectrumMarkers(warped))};
    const std::vector<talas::runtime::TableResource> tables{
        {"Tables/Guide", guideTable.triples}, {"Tables/Warped", warpedTable.triples}};
    const talas::project::ProjectDocument empty;

    const auto [left, right] = playedExample(talas::app::spectrumMixExampleSound(empty, "voice", guided, warping), tables, 0.0);
    const std::span leftSettled{left.data() + 1200, 2400};
    const std::span rightSettled{right.data() + 1200, 2400};
    require(toneLevel(leftSettled, 440.0) > 0.01 && toneLevel(leftSettled, 660.0) < 0.1 * toneLevel(leftSettled, 440.0)
            && toneLevel(rightSettled, 660.0) > 0.01
            && toneLevel(rightSettled, 440.0) < 0.1 * toneLevel(rightSettled, 660.0),
        "the mix should play the guide left and the warped right");

    const auto guideEnd = playedExample(talas::app::spectrumMorphExampleSound(empty, "voice", guided, warping), tables, 0.0).first;
    const auto warpedEnd = playedExample(talas::app::spectrumMorphExampleSound(empty, "voice", guided, warping), tables, 1.0).first;
    const std::span atZero{guideEnd.data() + 1200, 2400};
    const std::span atOne{warpedEnd.data() + 1200, 2400};
    require(toneLevel(atZero, 440.0) > 0.01 && toneLevel(atZero, 660.0) < 0.1 * toneLevel(atZero, 440.0),
        "the morph should be the guide spectrum at Morph 0: " + std::to_string(toneLevel(atZero, 440.0)) + " "
            + std::to_string(toneLevel(atZero, 660.0)));
    require(toneLevel(atOne, 660.0) > 0.01 && toneLevel(atOne, 440.0) < 0.1 * toneLevel(atOne, 660.0),
        "the morph should be the warped spectrum at Morph 1");
}

// The guide keeps its timing;
// the warped spectrum is stretched so that its marked features line up with
// the guide's. The guide changes from 440 to 550 hz at 50 ms, the warped
// from 660 to 770 hz at 120 ms, each marked there; in the mix the warped
// changes with the guide.
void testTheWarpedSpectrumLinesUpWithTheGuide()
{
    const auto changing = [](double first, double second, std::uint64_t at) {
        std::vector<talas::project::PartialTrackFrame> frames;
        for (std::uint64_t index = 0; index < 20; ++index)
        {
            const auto fundamental = index < at ? first : second;
            frames.push_back({1, index * 480, {{1, 0.5, fundamental, 0.0}, {2, 0.25, fundamental * 2.0, 0.0}}});
        }
        return talas::project::PartialTrackAsset{sampleRate, 9600, std::move(frames)};
    };
    const auto guide = changing(440.0, 550.0, 5);
    const auto warped = changing(660.0, 770.0, 12);
    const auto guideTable = talas::audio::spectrumInRamTable(guide);
    const auto warpedTable = talas::audio::spectrumInRamTable(warped);
    const talas::app::SynchronizedSpectrum guided{"Tables/Guide", guideTable.frames, guideTable.partials,
        talas::app::spectrumTimePoints(guide, {{"start", 0}, {"change", 5}, {"end", 19}})};
    const talas::app::SynchronizedSpectrum warping{"Tables/Warped", warpedTable.frames, warpedTable.partials,
        talas::app::spectrumTimePoints(warped, {{"start", 0}, {"change", 12}, {"end", 19}})};
    const std::vector<talas::runtime::TableResource> tables{
        {"Tables/Guide", guideTable.triples}, {"Tables/Warped", warpedTable.triples}};
    const auto [left, right] = playedExample(talas::app::spectrumMixExampleSound({}, "voice", guided, warping), tables, 0.0, 9600);
    const std::span guideBefore{left.data() + 240, 1440};
    const std::span guideAfter{left.data() + 3360, 1920};
    const std::span warpedAfter{right.data() + 3360, 1920};
    require(toneLevel(guideBefore, 440.0) > 2.0 * toneLevel(guideBefore, 550.0)
            && toneLevel(guideAfter, 550.0) > 2.0 * toneLevel(guideAfter, 440.0),
        "the guide spectrum's timing should not be warped");
    require(toneLevel(warpedAfter, 770.0) > 2.0 * toneLevel(warpedAfter, 660.0),
        "the warped spectrum's marked change should line up with the guide's: "
            + std::to_string(toneLevel(warpedAfter, 770.0)) + " " + std::to_string(toneLevel(warpedAfter, 660.0)));
}

// `seconds` long of samples from a time.
std::span<const double> stretchOf(const std::vector<double>& audio, double from, double seconds)
{
    const auto start = static_cast<std::size_t>(std::lround(from * sampleRate));
    const auto length = static_cast<std::size_t>(std::lround(seconds * sampleRate));
    require(start + length <= audio.size(), "the stretch should lie within the audio");
    return {audio.data() + start, length};
}

// Whether a frequency sounds in samples, and another, if given, is at least
// ten times quieter.
bool heard(std::span<const double> samples, double frequency, double quieter = 0.0)
{
    const auto level = toneLevel(samples, frequency);
    return level > 0.05 && (quieter <= 0.0 || toneLevel(samples, quieter) < 0.1 * level);
}

// A region copied from the spectrum editor joins the
// Sounds as a SumOfSines, which time-stretches and frequency-scales it.
// The spectrum holds track 1 at 400 hz for its first 0.2 s, track 2 at
// 700 hz after it, and track 3 at 1000 hz throughout; the region copied is
// tracks 1 and 2 from 0.1 to 0.3 s, 400 hz for 0.1 s and then 700 hz. Its
// !Stretch of 2 moves the change to 0.2 s in, its !FreqScale of 1.5 moves
// 400 and 700 hz to 600 and 1050 hz, and track 3, not copied, stays out.
void testACopiedRegionStretchesAndScales()
{
    talas::project::PartialTrackAsset spectrum{sampleRate, 19200, {}};
    for (std::uint64_t index = 0; index < 40; ++index)
    {
        const auto at = index * 480;
        const auto early = at < 9600;
        spectrum.frames.push_back(
            {1, at, {{early ? std::uint64_t{1} : std::uint64_t{2}, 0.4, early ? 400.0 : 700.0, 0.0}, {3, 0.3, 1000.0, 0.0}}});
    }
    talas::app::PartialTrackSelection selection;
    for (const auto& frame : spectrum.frames)
    {
        for (const auto& point : frame.points)
        {
            if (point.trackId != 3 && frame.sourceFrame >= 4800 && frame.sourceFrame <= 14400)
                selection.add(point.trackId, frame.sourceFrame);
        }
    }
    const auto region = talas::app::partialTrackRegionAsset(spectrum, selection);
    require(region.has_value(), "the region should be copied");
    const auto table = talas::audio::spectrumInRamTable(*region);
    const auto sines = talas::app::spectrumSumOfSinesSound({}, "Tables/Region", table.frames, table.partials, table.durationSeconds);
    require(table.partials == 2 && sines.resources.size() == 1 && sines.resources.front().parameterId == "analysis"
            && sines.controls.size() == 2 && sines.controls[0].eventName == "Stretch" && sines.controls[0].value == 1.0
            && sines.controls[1].eventName == "FreqScale" && sines.controls[1].value == 1.0,
        "the region's SumOfSines should read its table, with !Stretch and !FreqScale at 1 to start");
    const std::vector<talas::runtime::TableResource> tables{{"Tables/Region", table.triples}};
    const auto play = [&](double stretch, double scale) {
        return playedWithValues(sines, tables, {{"Stretch", stretch}, {"FreqScale", scale}}, 24000).first;
    };
    const auto plain = play(1.0, 1.0);
    require(heard(stretchOf(plain, 0.02, 0.06), 400.0, 700.0) && heard(stretchOf(plain, 0.12, 0.06), 700.0, 400.0)
            && toneLevel(stretchOf(plain, 0.02, 0.06), 1000.0) < 0.01,
        "the region should play its own tracks and times alone");
    const auto stretched = play(2.0, 1.0);
    require(heard(stretchOf(stretched, 0.12, 0.06), 400.0, 700.0) && heard(stretchOf(stretched, 0.27, 0.06), 700.0, 400.0),
        "a !Stretch of 2 should play the region over twice its time");
    const auto scaled = play(1.0, 1.5);
    require(heard(stretchOf(scaled, 0.02, 0.06), 600.0, 400.0) && heard(stretchOf(scaled, 0.12, 0.06), 1050.0, 700.0),
        "a !FreqScale of 1.5 should scale the region's frequencies by 1.5");
    const auto both = play(2.0, 1.5);
    require(heard(stretchOf(both, 0.12, 0.06), 600.0, 1050.0) && heard(stretchOf(both, 0.27, 0.06), 1050.0, 600.0),
        "the region should stretch and scale at once");
}

// The single pitch entered for a quasi-harmonic
// analysis of one instrument tone lets a MIDI keyboard play the
// resynthesis, its oscillators repitched to each key; speech or a sample of
// several pitches takes middle C, 60 nn, as its pitch, so that middle C
// plays the resynthesis at its original pitch and other keys move it up or
// down. The example SumOfSines keeps the pitch entered as the analysis's
// (AnalysisFrequency0) and plays at it; with !KeyPitch in its Frequency0
//, the key of the pitch
// entered plays the same samples, and the key an octave up the harmonics
// an octave up.
void testTheKeyboardRepitchesAHarmonicAnalysis()
{
    // Two frames of three harmonics' amplitudes.
    const std::vector<talas::runtime::TableResource> tables{{"Tables/Voice h", {0.4, 0.2, 0.1, 0.4, 0.2, 0.1}}};
    for (const auto& [pitchHz, noteNumber] : std::vector<std::pair<double, double>>{{440.0, 69.0}, {261.6255653, 60.0}})
    {
        const auto examples = talas::app::harmonicAnalysisExampleSounds({}, "Tables/Voice h", 2, 3, pitchHz, 1.0);
        require(!examples.empty(), "the analysis should leave its example Sounds");
        const auto asLeft = playedWithValues(examples.front(), tables, {}, 4800).first;
        require(heard(stretchOf(asLeft, 0.0, 0.1), pitchHz), "the example should play at the pitch entered");
        auto keyboard = examples.front();
        for (auto& node : keyboard.definition.graph.nodes)
        {
            if (node.instanceId == "sines")
                node.parameters.push_back(expression("frequency0", "!KeyPitch"));
        }
        const auto keyed = [&](double key) {
            talas::runtime::EventValueBinding pitch;
            pitch.name = "KeyPitch";
            pitch.value = key;
            pitch.domain = talas::expr::ValueDomain::NoteNumber;
            return playedWithValues(keyboard, tables, {}, 4800, {pitch}).first;
        };
        const auto atThePitch = keyed(noteNumber);
        double apart = 0.0;
        for (std::size_t frame = 0; frame < asLeft.size(); ++frame)
            apart = std::max(apart, std::abs(atThePitch[frame] - asLeft[frame]));
        require(apart < 1.0e-6,
            "the key of the pitch entered (" + std::to_string(noteNumber)
                + " nn) should play the resynthesis at its original pitch: " + std::to_string(apart) + " apart");
        const auto octaveUp = keyed(noteNumber + 12.0);
        const auto fifthDown = keyed(noteNumber - 7.0);
        require(heard(stretchOf(octaveUp, 0.0, 0.1), 2.0 * pitchHz, pitchHz)
                && heard(stretchOf(fifthDown, 0.0, 0.1), pitchHz * std::pow(2.0, -7.0 / 12.0), pitchHz),
            "other keys should repitch the oscillators to match them");
    }
}

// Both kinds of spectrum file, straight and
// quasi-harmonic, can be time-scaled, pitch-shifted and morphed. A tone of
// 220 hz for 0.2 s and then 330 hz, analysed into a straight spectrum and a
// quasi-harmonic one, plays through the SumOfSines with !Stretch 2 (the
// change heard 0.4 s in) and !FreqScale 2 (220 hz an octave up); and each
// kind morphs with the same kind of spectrum of a steady 180 hz tone: the
// first tone at Morph 0, the second at 1, and, quasi-harmonic, a
// fundamental of 200 hz halfway.
void testBothKindsOfSpectrumScaleShiftAndMorph()
{
    const auto tone = [](double first, double second) {
        std::vector<double> audio(19200, 0.0);
        for (std::size_t frame = 0; frame < audio.size(); ++frame)
        {
            const auto t = static_cast<double>(frame) / sampleRate;
            const auto fundamental = frame < 9600 ? first : second;
            for (int harmonic = 1; harmonic <= 5; ++harmonic)
                audio[frame] += 0.3 / harmonic * std::sin(2.0 * std::numbers::pi * fundamental * harmonic * t);
        }
        return audio;
    };
    // The straight spectrum and the quasi-harmonic one of a sample, as the
    // Spectral Analysis Tool makes them.
    const auto analysed = [](const std::vector<double>& audio) {
        talas::audio::PartialTrackAudioAnalysisSettings settings;
        settings.lowestFrequencyHz = 87.31;
        settings.kind = talas::audio::PartialTrackAnalysisKind::Straight;
        const auto straight = talas::audio::analyzeMonoAudioPartialTracks(audio, sampleRate, settings);
        require(straight.ok(), "the straight analysis should run");
        const auto pitches = talas::audio::framePitches(straight.asset, settings.lowestFrequencyHz);
        const auto trace = talas::audio::traceFundamental(straight.asset, pitches, 0.2, settings.lowestFrequencyHz);
        require(!trace.empty(), "the fundamental should be traced");
        return std::pair{straight.asset, talas::audio::harmonicSpectrum(straight.asset, trace)};
    };
    const auto [straightA, harmonicA] = analysed(tone(220.0, 330.0));
    const auto [straightB, harmonicB] = analysed(tone(180.0, 180.0));
    for (const auto* kind : {"straight", "quasi-harmonic"})
    {
        const auto isStraight = std::string{kind} == "straight";
        const auto& a = isStraight ? straightA : harmonicA;
        const auto& b = isStraight ? straightB : harmonicB;
        const auto tableA = talas::audio::spectrumInRamTable(a);
        const auto tableB = talas::audio::spectrumInRamTable(b);
        require(tableA.frames > 10 && tableA.partials > 2, std::string{"the "} + kind + " spectrum should have frames and tracks");
        const auto sines = talas::app::spectrumSumOfSinesSound({}, "Tables/A", tableA.frames, tableA.partials, tableA.durationSeconds);
        const std::vector<talas::runtime::TableResource> tables{{"Tables/A", tableA.triples}, {"Tables/B", tableB.triples}};
        const auto play = [&](double stretch, double scale) {
            return playedWithValues(sines, tables, {{"Stretch", stretch}, {"FreqScale", scale}}, 33600).first;
        };
        const auto plain = play(1.0, 1.0);
        require(heard(stretchOf(plain, 0.05, 0.1), 220.0, 330.0) && heard(stretchOf(plain, 0.25, 0.1), 330.0, 220.0),
            std::string{"the "} + kind + " spectrum should play the tone, 220 hz and then 330 hz");
        const auto stretched = play(2.0, 1.0);
        require(heard(stretchOf(stretched, 0.25, 0.1), 220.0, 330.0) && heard(stretchOf(stretched, 0.5, 0.1), 330.0, 220.0),
            std::string{"the "} + kind + " spectrum should be time-scaled, the change heard 0.4 s in");
        const auto shifted = play(1.0, 2.0);
        require(heard(stretchOf(shifted, 0.05, 0.1), 440.0, 220.0) && heard(stretchOf(shifted, 0.25, 0.1), 660.0, 330.0),
            std::string{"the "} + kind + " spectrum should be pitch-shifted an octave up");

        const auto markers = [](const talas::project::PartialTrackAsset& spectrum) {
            return talas::app::spectrumTimePoints(spectrum, {{"start", 0}, {"end", spectrum.frames.size() - 1}});
        };
        const talas::app::SynchronizedSpectrum guide{"Tables/A", tableA.frames, tableA.partials, markers(a)};
        const talas::app::SynchronizedSpectrum warped{"Tables/B", tableB.frames, tableB.partials, markers(b)};
        const auto morph = talas::app::spectrumMorphExampleSound({}, "tones", guide, warped);
        const auto atZero = playedExample(morph, tables, 0.0, 9600).first;
        const auto atOne = playedExample(morph, tables, 1.0, 9600).first;
        require(heard(stretchOf(atZero, 0.05, 0.1), 220.0, 180.0) && heard(stretchOf(atOne, 0.05, 0.1), 180.0, 220.0),
            std::string{"the "} + kind + " spectrum should morph, the first tone at Morph 0 and the second at 1");
        if (!isStraight)
        {
            const auto halfway = playedExample(morph, tables, 0.5, 9600).first;
            require(heard(stretchOf(halfway, 0.05, 0.1), 200.0, 220.0) && toneLevel(stretchOf(halfway, 0.05, 0.1), 180.0)
                        < 0.1 * toneLevel(stretchOf(halfway, 0.05, 0.1), 200.0),
                "halfway the quasi-harmonic morph's fundamental should lie between the two, at 200 hz");
        }
    }
}
}

int main()
{
    try
    {
        testEachTrackIsASlotOfTriples();
        testTheTableIsWrittenAsATableResource();
        testSpectrumInRamPlaysTheSpectrum();
        testTheSumOfSinesIsASpectrumInRamControllingAnOscillatorBank();
        testTheMixAndTheMorph();
        testTheWarpedSpectrumLinesUpWithTheGuide();
        testACopiedRegionStretchesAndScales();
        testTheKeyboardRepitchesAHarmonicAnalysis();
        testBothKindsOfSpectrumScaleShiftAndMorph();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
