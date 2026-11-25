#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/SystemWavetables.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The Wavetables folder gives every project standard tables by name
//: a resource field that names Sine or Hann plays it
// without the project supplying a file.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<double> table(std::string_view name)
{
    auto found = talas::dsp::systemWavetable(name);
    require(found.has_value(), std::string{name} + " should be a system wavetable");
    require(found->size() == 4096, std::string{name} + " should have 4096 entries");
    return *found;
}

void testShapes()
{
    for (const auto name : talas::dsp::systemWavetableNames())
    {
        for (const auto value : table(name))
            require(std::isfinite(value), std::string{name} + " should be finite");
    }
    const auto sine = table("Sine");
    require(sine[0] == 0.0 && sine[1024] == 1.0 && std::abs(sine[3072] + 1.0) < 1e-15,
        "Sine should be one cycle starting at zero");
    const auto cosine = table("Cosine");
    require(cosine[0] == 1.0 && std::abs(cosine[2048] + 1.0) < 1e-15,
        "Cosine should be one cycle starting at one");
    const auto triangle = table("Triangle");
    require(triangle[0] == 0.0 && triangle[1024] == 1.0 && triangle[2048] == 0.0
            && triangle[3072] == -1.0,
        "Triangle should rise to 1 at a quarter and fall to -1 at three quarters");
    const auto fullRamp = table("FullRamp");
    require(fullRamp.front() == -1.0 && fullRamp.back() == 1.0,
        "FullRamp should be a straight line from -1 up to 1");
    const auto ramp = table("Ramp");
    require(ramp.front() == 0.0 && ramp.back() == 1.0, "Ramp should rise from 0 to 1");
    const auto linear = table("LinearEnvelope");
    require(linear.front() == 0.0 && linear.back() == 0.0
            && *std::ranges::max_element(linear) > 0.999,
        "LinearEnvelope should rise from 0 to 1 and fall back");
    const auto hann = table("Hann");
    require(hann.front() == 0.0 && std::abs(hann.back()) < 1e-15
            && *std::ranges::max_element(hann) > 0.999,
        "Hann should be zero at its first and last entries");
    const auto hamming = table("Hamming");
    require(std::abs(hamming.front() - 0.08) < 1e-15, "Hamming should end at 0.08");
    const auto gaussian = table("Gaussian");
    require(gaussian.front() < 1e-3 && gaussian.back() < 1e-3
            && *std::ranges::max_element(gaussian) > 0.999,
        "Gaussian should be a bell rising from near zero to one");
    require(std::ranges::all_of(table("Rectangular"), [](double value) { return value == 1.0; })
            && std::ranges::all_of(table("Zero"), [](double value) { return value == 0.0; }),
        "Rectangular should be one and Zero zero");
    require(!talas::dsp::systemWavetable("Private").has_value()
            && !talas::dsp::systemWavetable("sine").has_value(),
        "only the standard table names should resolve");
}

// The sine and cosine amplitudes of a table's harmonic.
std::pair<double, double> harmonic(const std::vector<double>& values, std::size_t number)
{
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto phase = 2.0 * std::numbers::pi * static_cast<double>(number * index)
            / static_cast<double>(values.size());
        sine += values[index] * std::sin(phase);
        cosine += values[index] * std::cos(phase);
    }
    const auto scale = 2.0 / static_cast<double>(values.size());
    return {sine * scale, cosine * scale};
}

double peak(const std::vector<double>& values)
{
    return std::ranges::max(values, {}, [](double value) { return std::abs(value); });
}

// Waveforms named by their harmonics: Saw21 and Square21,
// a sawtooth and a square of 21 harmonics, and Buzz, a pulse of equal
// harmonics; and GlottalPulse, one opening and closing of the glottis.
void testHarmonicWaveforms()
{
    const auto saw = table("Saw21");
    const auto square = table("Square21");
    const auto buzz = table("Buzz");
    require(std::abs(std::abs(peak(saw)) - 1.0) < 1e-15 && std::abs(std::abs(peak(square)) - 1.0) < 1e-15
            && std::abs(buzz[0] - 1.0) < 1e-15 && std::abs(peak(buzz) - 1.0) < 1e-15,
        "Saw21, Square21 and Buzz should peak at 1");
    const auto sawFirst = harmonic(saw, 1).first;
    const auto squareFirst = harmonic(square, 1).first;
    const auto buzzFirst = harmonic(buzz, 1).second;
    for (std::size_t number = 1; number <= 30; ++number)
    {
        const auto [sawSine, sawCosine] = harmonic(saw, number);
        const auto [squareSine, squareCosine] = harmonic(square, number);
        const auto [buzzSine, buzzCosine] = harmonic(buzz, number);
        const auto present = number <= 21;
        const auto odd = number % 2 == 1;
        const auto ratio = 1.0 / static_cast<double>(number);
        require(std::abs(sawSine - (present ? sawFirst * ratio : 0.0)) < 1e-12 && std::abs(sawCosine) < 1e-12,
            "Saw21's harmonic " + std::to_string(number) + " should be 1/n of its first up to the 21st");
        require(std::abs(squareSine - (present && odd ? squareFirst * ratio : 0.0)) < 1e-12
                && std::abs(squareCosine) < 1e-12,
            "Square21's harmonic " + std::to_string(number) + " should be 1/n of its first, odd ones only");
        require(std::abs(buzzCosine - (present ? buzzFirst : 0.0)) < 1e-12 && std::abs(buzzSine) < 1e-12,
            "Buzz's harmonic " + std::to_string(number) + " should equal its first up to the 21st");
    }
    require(sawFirst < 0.0 && saw[1024] < 0.0 && saw[3072] > 0.0 && std::abs(saw[2048]) < 1e-12,
        "Saw21 should rise through its cycle");
    require(squareFirst > 0.0 && square[1024] > 0.8 && square[3072] < -0.8,
        "Square21 should be high for its first half and low for its second");
    const auto glottal = table("GlottalPulse");
    require(glottal[0] == 0.0 && peak(glottal) > 0.999
            && std::ranges::all_of(glottal, [](double value) { return value >= 0.0 && value <= 1.0; })
            && std::all_of(glottal.begin() + 2294, glottal.end(), [](double value) { return value == 0.0; })
            && glottal[1000] > glottal[500] && glottal[2000] < glottal[1700],
        "GlottalPulse should open, close and stay closed for the rest of its cycle");
    require(!talas::dsp::systemWavetable("Saw").has_value() && !talas::dsp::systemWavetable("Saw32").has_value(),
        "only the named harmonic tables should resolve");
}

// The control tables: Exponential rises from 0 to 1,
// slowly then fast; ExponRev falls back the same way; FourExpons repeats
// ExponRev four times; Random is uniform from -1 to 1.
void testControlTables()
{
    const auto rising = table("Exponential");
    const auto falling = table("ExponRev");
    require(rising.front() == 0.0 && std::abs(rising.back() - 1.0) < 1e-15 && rising[2048] < 0.05
            && std::ranges::is_sorted(rising),
        "Exponential should rise from 0 to 1, slowly then fast");
    for (std::size_t index = 0; index < 4096; ++index)
        require(falling[index] == rising[4095 - index], "ExponRev should be Exponential reversed");
    const auto four = table("FourExpons");
    for (const std::size_t start : {0U, 1024U, 2048U, 3072U})
    {
        require(std::abs(four[start] - 1.0) < 1e-15 && std::abs(four[start + 1023]) < 1e-15
                && std::is_sorted(four.begin() + static_cast<std::ptrdiff_t>(start),
                    four.begin() + static_cast<std::ptrdiff_t>(start) + 1024, std::greater<>{}),
            "FourExpons should fall from 1 to 0 in each quarter, from " + std::to_string(start));
    }
    const auto random = table("Random");
    const auto mean = std::accumulate(random.begin(), random.end(), 0.0) / 4096.0;
    require(std::ranges::all_of(random, [](double value) { return value >= -1.0 && value <= 1.0; })
            && std::abs(mean) < 0.05 && *std::ranges::max_element(random) > 0.99
            && *std::ranges::min_element(random) < -0.99 && random == table("Random"),
        "Random should be uniform from -1 to 1, the same each time");
}

// A waveform is one cycle, so a Sample looping all of it plays it on
//.
void testASampleLoopsAWaveform()
{
    Parameter sample;
    sample.parameterId = "sample";
    sample.resource = "Saw21";
    Parameter loop;
    loop.parameterId = "setLoop";
    loop.boolean = true;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"sample", "talas.sample", {sample, loop}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"sample", "audioOut", "out", "audioIn"}};
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const talas::runtime::RenderSettings settings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "a Sample on Saw21 should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 3 * 4096);
    require(offline.ok(), "a looping Sample on Saw21 should render");
    const auto saw = table("Saw21");
    for (const std::size_t frame : {100U, 1024U, 3000U})
    {
        require(std::abs(offline.mono[frame] - saw[frame]) < 1e-6
                && std::abs(offline.mono[2 * 4096 + frame] - saw[frame]) < 1e-6,
            "a looping Sample on Saw21 should play its cycle again at frame " + std::to_string(frame));
    }
}

// The Waveshapers are output sample as a function of input
// sample: a Waveshaper on ATAN20 or ATAN40 raises a quiet sample by the
// arctangent and keeps a full one at full.
void testWaveshapersShapeSamples()
{
    const auto shaped = [](const std::string& curve, const std::string& input) {
        Parameter value;
        value.parameterId = "value";
        value.expression = input;
        Parameter shape;
        shape.parameterId = "shapingFunction";
        shape.resource = curve;
        talas::project::ProjectDocument document;
        document.graph.nodes = {
            {"in", "talas.constant", {value}},
            {"shape", "talas.waveshaper", {shape}},
            {"out", "talas.audioOutput", {}},
        };
        document.graph.connections = {{"in", "value", "shape", "audioIn"}, {"shape", "audioOut", "out", "audioIn"}};
        const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {48000.0, 256});
        require(compiled.ok(), "a Waveshaper on " + curve + " should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
        const auto offline = talas::runtime::renderOffline(*compiled.graph, 256);
        require(offline.ok(), "a Waveshaper on " + curve + " should render");
        return offline.mono[100];
    };
    for (const auto& [curve, gain] : {std::pair<std::string, double>{"ATAN20", 20.0}, {"ATAN40", 40.0}})
    {
        const auto atan = table(curve);
        require(std::abs(atan.front() + 1.0) < 1e-15 && std::abs(atan.back() - 1.0) < 1e-15
                && std::ranges::is_sorted(atan),
            curve + " should rise from -1 to 1");
        const auto quiet = shaped(curve, "0.025");
        require(std::abs(quiet - std::atan(gain * 0.025) / std::atan(gain)) < 1e-4
                && quiet > 10.0 * 0.025 && std::abs(shaped(curve, "-1") + 1.0) < 1e-9
                && std::abs(shaped(curve, "1") - 1.0) < 1e-9,
            "a Waveshaper on " + curve + " should raise a quiet sample by its arctangent, not to "
                + std::to_string(quiet));
    }
    require(shaped("ATAN40", "0.025") > shaped("ATAN20", "0.025"),
        "ATAN40 should raise a quiet sample more than ATAN20");
}

// An Oscillator stepping once through its 4096-entry table each 4096
// frames plays the table itself.
talas::project::ProjectDocument oscillatorOn(const std::string& wavetable)
{
    Parameter frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "11.71875 hz";
    Parameter resource;
    resource.parameterId = "wavetable";
    resource.resource = wavetable;
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"osc", "talas.oscillator", {frequency, resource}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    return document;
}

void testAnOscillatorPlaysASystemWavetable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const talas::runtime::RenderSettings settings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(oscillatorOn("Cosine"), registry, settings);
    require(compiled.ok(), "an Oscillator on Cosine should compile without a supplied table: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 2048);
    require(offline.ok(), "an Oscillator on Cosine should render offline");
    for (const std::size_t frame : {0U, 512U, 1024U, 2047U})
    {
        require(std::abs(offline.mono[frame]
                    - std::cos(2.0 * std::numbers::pi * static_cast<double>(frame) / 4096.0))
                < 1e-9,
            "the Oscillator should play Cosine at frame " + std::to_string(frame));
    }
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "an Oscillator on Cosine should prepare for realtime: "
        + std::string{session.error()});
    std::vector<double> left(256);
    std::vector<double> right(256);
    require(session.render({}, left, right) && left[0] == offline.mono[0]
            && left[255] == offline.mono[255],
        "an Oscillator on Cosine should play alike in realtime");
}

void testASuppliedTableOfTheSameNameWins()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::RenderSettings settings{48000.0, 256};
    settings.tableResources = {{"Sine", std::vector<double>(4096, 0.5)}};
    const auto compiled = talas::runtime::compileProject(oscillatorOn("Sine"), registry, settings);
    require(compiled.ok(), "an Oscillator on a supplied Sine should compile");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 64);
    require(offline.ok() && std::ranges::all_of(offline.mono, [](double value) { return value == 0.5; }),
        "the project's own Sine should replace the standard one");
}

void testAnUnknownTableIsStillMissing()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const talas::runtime::RenderSettings settings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(oscillatorOn("Buzz128"), registry, settings);
    const auto offline = compiled.ok()
        ? talas::runtime::renderOffline(*compiled.graph, 64)
        : talas::runtime::OfflineRenderResult{};
    require(!compiled.ok() || !offline.ok(),
        "a standard table this project does not compute should stay missing");
}

// Grains need a waveform and an envelope; the standard Sine and Hann serve.
void testAGrainCloudPlaysOnSystemWavetables()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    Parameter waveform;
    waveform.parameterId = "waveform";
    waveform.resource = "Sine";
    Parameter envelope;
    envelope.parameterId = "grainEnv";
    envelope.resource = "Hann";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"grains", "talas.grainCloud", {waveform, envelope}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"grains", "leftOut", "out", "audioIn"},
        {"grains", "rightOut", "out", "rightIn"},
    };
    const talas::runtime::RenderSettings settings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "a GrainCloud on Sine and Hann should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 9600);
    require(offline.ok(), "a GrainCloud on Sine and Hann should render");
    require(std::ranges::any_of(offline.mono, [](double value) { return std::abs(value) > 0.01; }),
        "a GrainCloud on Sine and Hann should be heard");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "a GrainCloud on Sine and Hann should prepare for realtime: "
        + std::string{session.error()});
}

// Fields that take an analysis, an RE table or a terrain default to
// computed material of those shapes, as Spectra and RE files ship beside
// the wavetables: a SpectrumInRAM given only a
// Frequency plays its harmonic analysis at that pitch.
void testComputedTablesPlay()
{
    const auto harmonics = talas::dsp::systemTable("HarmonicAmplitudes");
    require(harmonics.has_value() && harmonics->size() == 8, "HarmonicAmplitudes should hold eight partials");
    for (std::size_t partial = 0; partial < 8; ++partial)
    {
        require(std::abs((*harmonics)[partial] * static_cast<double>(partial + 1) - harmonics->front()) < 1.0e-15,
            "HarmonicAmplitudes should fall as 1/n");
    }
    require(std::abs(std::accumulate(harmonics->begin(), harmonics->end(), 0.0) - 1.0) < 1.0e-12,
        "HarmonicAmplitudes should sum to 1");
    const auto spectrum = talas::dsp::systemTable("HarmonicSpectrum");
    require(spectrum.has_value() && spectrum->size() == 24, "HarmonicSpectrum should hold eight partials");
    double amplitudes = 0.0;
    for (std::size_t partial = 0; partial < 8; ++partial)
    {
        amplitudes += (*spectrum)[partial * 3];
        require((*spectrum)[partial * 3 + 1] == static_cast<double>(partial + 1) && (*spectrum)[partial * 3 + 2] == 0.0,
            "HarmonicSpectrum's partials should be the harmonics, without bandwidth");
    }
    require(std::abs(amplitudes - 1.0) < 1.0e-12, "HarmonicSpectrum's amplitudes should sum to 1");
    const auto resonance = talas::dsp::systemTable("Resonance");
    require(resonance.has_value() && resonance->size() == 3 && resonance->front() == 2.0
            && std::abs((*resonance)[1]) < 1.0 && std::abs((*resonance)[2]) < 1.0,
        "Resonance should be an RE table of order 2 with stable reflection coefficients");
    const auto terrain = talas::dsp::systemTable("Terrain");
    require(terrain.has_value() && terrain->size() == 64
            && std::ranges::all_of(*terrain, [](double value) { return std::abs(value) <= 1.0; }),
        "Terrain should be 8 by 8 samples within -1 and 1");
    require(talas::dsp::systemTable("Sine") == talas::dsp::systemWavetable("Sine"),
        "the system wavetables should be system tables too");

    Parameter frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "220 hz";
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"partials", "talas.spectrumInRam", {frequency}},
        {"bank", "talas.oscillatorBank", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"partials", "spectrumOut", "bank", "spectrum"},
        {"bank", "audioOut", "out", "audioIn"},
    };
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const talas::runtime::RenderSettings settings{48000.0, 256};
    const auto compiled = talas::runtime::compileProject(document, registry, settings);
    require(compiled.ok(), "a SpectrumInRAM with its default analysis should compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 9600);
    require(offline.ok() && std::ranges::any_of(offline.mono, [](double value) { return std::abs(value) > 0.01; }),
        "a SpectrumInRAM with its default analysis should be heard");
}
}

// The Windows are symmetric waveshapes, for control signals
// and amplitude envelopes.
void testWindowsAreSymmetric()
{
    for (const auto* name : {"Hamming", "Hann", "LinearEnvelope", "Gaussian"})
    {
        const auto window = table(name);
        for (std::size_t index = 0; index < window.size(); ++index)
            require(std::abs(window[index] - window[window.size() - 1 - index]) < 1.0e-12,
                std::string{""} + name + " should be symmetric");
    }
}

int main()
{
    try
    {
        testShapes();
        testHarmonicWaveforms();
        testControlTables();
        testWaveshapersShapeSamples();
        testASampleLoopsAWaveform();
        testAnOscillatorPlaysASystemWavetable();
        testASuppliedTableOfTheSameNameWins();
        testAnUnknownTableIsStillMissing();
        testAGrainCloudPlaysOnSystemWavetables();
        testComputedTablesPlay();
        testWindowsAreSymmetric();
        std::cout << "System wavetables tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
