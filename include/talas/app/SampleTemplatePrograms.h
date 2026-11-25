#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// The sample editor's template Programs: the
// Smalltalk code of a wavetable generator, to read, change and run. A
// program answers the wavetable's samples, one for each of `size`, the
// number of samples asked for; changed, it can be copied to a text file,
// as the templates themselves are not saved.
namespace talas::app
{
enum class SampleTemplateProgram
{
    Fourier,
    InputOutputCharacteristic,
    Polynomial,
    Random,
    // A program for each of the template waveform generators.
    Buzz,
    LineSegments,
    ExponentialSegments,
    CubicSpline,
    ImpulseResponse,
};

[[nodiscard]] const std::vector<SampleTemplateProgram>& sampleTemplatePrograms();
[[nodiscard]] std::string_view sampleTemplateProgramName(SampleTemplateProgram program);
[[nodiscard]] std::string_view sampleTemplateProgramSource(SampleTemplateProgram program);

struct SampleTemplateProgramResult
{
    std::vector<double> samples;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty(); }
};

// Runs a program for `size` samples; its answer, an Array of that many
// numbers, scaled down to a peak of 1 where it goes beyond.
[[nodiscard]] SampleTemplateProgramResult runSampleTemplateProgram(std::string_view source, std::size_t size);
}
