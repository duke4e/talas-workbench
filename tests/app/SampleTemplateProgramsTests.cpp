#include "talas/app/SampleEditorModel.h"
#include "talas/app/SampleTemplatePrograms.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>

// The sample editor's template Programs, the
// Smalltalk code of wavetable generators, read, changed and run.
namespace
{
using talas::app::SampleTemplateProgram;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::app::SampleTemplateProgramResult run(SampleTemplateProgram program, std::size_t size)
{
    return talas::app::runSampleTemplateProgram(talas::app::sampleTemplateProgramSource(program), size);
}

void testTheProgramsMakeWavetables()
{
    require(talas::app::sampleTemplatePrograms().size() == 9
            && talas::app::sampleTemplateProgramName(SampleTemplateProgram::Fourier) == "Fourier Program"
            && talas::app::sampleTemplateProgramName(SampleTemplateProgram::InputOutputCharacteristic)
                == "InputOutputCharacteristic Program",
        "the Programs are listed under their templates' names");
    // Fourier: harmonics 1, 3 and 5 at 1, 0.333 and 0.2, a square wave's
    // beginning, within -1 and 1.
    const auto fourier = run(SampleTemplateProgram::Fourier, 256);
    require(fourier.ok() && fourier.samples.size() == 256, "the Fourier Program should give 256 samples: " + fourier.error);
    const auto expected = [](double phase) {
        return std::sin(phase) + 0.333 * std::sin(3 * phase) + 0.2 * std::sin(5 * phase);
    };
    const auto peak = std::ranges::max(fourier.samples, {}, [](double value) { return std::abs(value); });
    require(std::abs(fourier.samples[0]) < 1e-9 && std::abs(peak) <= 1.0 && std::abs(peak) > 0.9
            && std::abs(fourier.samples[64] / fourier.samples[32] - expected(std::numbers::pi / 2) / expected(std::numbers::pi / 4)) < 1e-6,
        "the Fourier Program should sum its harmonics");
    // A changed program: an inharmonic partial.
    auto source = std::string{talas::app::sampleTemplateProgramSource(SampleTemplateProgram::Fourier)};
    source.replace(source.find("#(1 3 5)"), 8, "#(1 3 5.5)");
    require(talas::app::runSampleTemplateProgram(source, 64).ok(), "a modified program should run");
    const auto random = run(SampleTemplateProgram::Random, 128);
    require(random.ok() && std::ranges::all_of(random.samples, [](double value) { return value >= -1.0 && value <= 1.0; })
            && run(SampleTemplateProgram::Random, 128).samples == random.samples,
        "the Random Program should give repeatable noise from -1 to 1: " + random.error);
    const auto line = run(SampleTemplateProgram::Polynomial, 5);
    require(line.ok() && line.samples == std::vector<double>{-1.0, -0.5, 0.0, 0.5, 1.0},
        "the Polynomial Program's 0 1 is the line from -1 to 1: " + line.error);
    const auto curve = run(SampleTemplateProgram::InputOutputCharacteristic, 9);
    require(curve.ok() && curve.samples[4] == 1.0 && curve.samples[0] < 1.0 && curve.samples[8] < 1.0
            && std::abs(curve.samples[0] - (0.5 + 0.5 / 4) / 1.0) < 1e-9,
        "the InputOutputCharacteristic Program compresses above its threshold: " + curve.error);
    require(!talas::app::runSampleTemplateProgram("#(1 2)", 3).ok() && !talas::app::runSampleTemplateProgram("1 +", 3).ok(),
        "a program that answers the wrong number of samples, or does not parse, is refused");

    // A program for each template waveform generator, giving
    // what the template gives with its own fields (scaled to a peak of 1).
    using talas::app::SampleTemplate;
    for (const auto& [program, generator] : {std::pair{SampleTemplateProgram::Buzz, SampleTemplate::Buzz},
             std::pair{SampleTemplateProgram::LineSegments, SampleTemplate::LineSegments},
             std::pair{SampleTemplateProgram::ExponentialSegments, SampleTemplate::ExponentialSegments},
             std::pair{SampleTemplateProgram::CubicSpline, SampleTemplate::CubicSpline},
             std::pair{SampleTemplateProgram::ImpulseResponse, SampleTemplate::ImpulseResponse}})
    {
        const auto name = std::string{talas::app::sampleTemplateProgramName(program)};
        const auto ran = run(program, 200);
        talas::app::SampleEditorModel editor(48000.0, 1, 0);
        const std::map<std::string, std::string> fields{{"Duration", "200 samp"}};
        require(ran.ok() && talas::app::applySampleTemplate(editor, generator, fields).ok() && editor.frames() == 200,
            "the " + name + " should run as its template does: " + ran.error);
        auto made = editor.channels()[0];
        const auto peak = std::ranges::max(made, {}, [](double value) { return std::abs(value); });
        if (std::abs(peak) > 1.0)
            for (auto& sample : made)
                sample /= std::abs(peak);
        for (std::size_t frame = 0; frame < made.size(); ++frame)
            require(std::abs(ran.samples[frame] - made[frame]) < 1.0e-9,
                "the " + name + " should give its template's samples at " + std::to_string(frame));
    }
}
}

int main()
{
    try
    {
        testTheProgramsMakeWavetables();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
