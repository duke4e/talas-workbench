#include "talas/app/SampleTemplatePrograms.h"

#include "talas/project/SmalltalkSubset.h"

#include <algorithm>
#include <cmath>

namespace talas::app
{
namespace
{
namespace smalltalk = talas::project::smalltalk;

// The program's one free name: the number of samples to make.
class ProgramHost final : public smalltalk::Host
{
public:
    explicit ProgramHost(std::size_t size) : size_(size) {}

    [[nodiscard]] std::uint32_t freshSeed() override { return 1; }

    [[nodiscard]] std::optional<smalltalk::Value> variable(std::string_view name) override
    {
        if (name == "size")
            return smalltalk::Value::of(static_cast<double>(size_));
        return std::nullopt;
    }

    [[nodiscard]] std::optional<smalltalk::Value> send(
        const smalltalk::Value&, std::string_view selector, std::span<const smalltalk::Value>, std::string& error) override
    {
        error = "A wavetable program has no object that understands " + std::string{selector};
        return std::nullopt;
    }

private:
    std::size_t size_;
};
}

const std::vector<SampleTemplateProgram>& sampleTemplatePrograms()
{
    static const std::vector<SampleTemplateProgram> all{SampleTemplateProgram::Fourier,
        SampleTemplateProgram::InputOutputCharacteristic, SampleTemplateProgram::Polynomial, SampleTemplateProgram::Random,
        SampleTemplateProgram::Buzz, SampleTemplateProgram::LineSegments, SampleTemplateProgram::ExponentialSegments,
        SampleTemplateProgram::CubicSpline, SampleTemplateProgram::ImpulseResponse};
    return all;
}

std::string_view sampleTemplateProgramName(SampleTemplateProgram program)
{
    switch (program)
    {
    case SampleTemplateProgram::Fourier: return "Fourier Program";
    case SampleTemplateProgram::InputOutputCharacteristic: return "InputOutputCharacteristic Program";
    case SampleTemplateProgram::Polynomial: return "Polynomial Program";
    case SampleTemplateProgram::Random: return "Random Program";
    case SampleTemplateProgram::Buzz: return "Buzz Program";
    case SampleTemplateProgram::LineSegments: return "Line Segments Program";
    case SampleTemplateProgram::ExponentialSegments: return "Exponential Segments Program";
    case SampleTemplateProgram::CubicSpline: return "Cubic Spline Program";
    case SampleTemplateProgram::ImpulseResponse: return "Impulse Response Program";
    }
    return {};
}

std::string_view sampleTemplateProgramSource(SampleTemplateProgram program)
{
    switch (program)
    {
    case SampleTemplateProgram::Fourier:
        return R"("Sums sine waves: each harmonic number, its amplitude and its phase
(in fractions of a cycle). Any number of harmonics, and harmonics that are
not whole numbers for an inharmonic spectrum."
| harmonics amplitudes phases |
harmonics := #(1 3 5).
amplitudes := #(1 0.333 0.2).
phases := #(0 0 0).
(1 to: size) collect: [:i |
	(1 to: harmonics size) inject: 0 into: [:sum :h |
		sum + ((amplitudes at: h)
			* (((i - 1) / size * (harmonics at: h) + (phases at: h)) * 2 * Float pi) sin)]])";
    case SampleTemplateProgram::InputOutputCharacteristic:
        return R"("A gain curve for a MultiplyingWaveshaper, input levels from -1 to 1:
unity below the threshold, and above it the level rising only a ratio's
part as fast, which compresses; a ratio below 1 expands."
| threshold ratio |
threshold := 0.5.
ratio := 4.
(1 to: size) collect: [:i |
	(((i - 1) / (size - 1) * 2 - 1) abs <= threshold)
		ifTrue: [1]
		ifFalse: [(threshold + ((((i - 1) / (size - 1) * 2 - 1) abs - threshold) / ratio))
			/ ((i - 1) / (size - 1) * 2 - 1) abs]])";
    case SampleTemplateProgram::Polynomial:
        return R"("A polynomial over x from -1 to 1, its coefficients from the constant
term up: 0 1 is the line from -1 to 1, 0 0 1 a parabola."
| coefficients |
coefficients := #(0 1).
(1 to: size) collect: [:i |
	(1 to: coefficients size) inject: 0 into: [:sum :k |
		sum + ((coefficients at: k) * (((i - 1) / (size - 1) * 2 - 1) raisedTo: k - 1))]])";
    case SampleTemplateProgram::Random:
        return R"("Uniform random values from -1 to 1, white noise; change the seed for
another noise, or shape each value for other kinds."
| random |
random := Random new seed: 1.
(1 to: size) collect: [:i | random next * 2 - 1])";
    case SampleTemplateProgram::Buzz:
        return R"("Harmonics sines from the lowest harmonic up, each ampBase raised to
its harmonic number times the gain, one cycle over the wavetable."
| harmonics lowest ampBase gain |
harmonics := 8.
lowest := 1.
ampBase := 0.8.
gain := 0.5.
(1 to: size) collect: [:i |
	(0 to: harmonics - 1) inject: 0 into: [:sum :k |
		sum + ((ampBase raisedTo: lowest + k) * gain * ((i - 1) / size * (lowest + k) * 2 * Float pi) sin)]])";
    case SampleTemplateProgram::LineSegments:
    case SampleTemplateProgram::ExponentialSegments:
        return program == SampleTemplateProgram::LineSegments
            ? R"("Straight lines from each end point to the next, each taking its
proportion of the wavetable."
| points proportions total |
points := #(0 1 0).
proportions := #(0.5 0.5).
total := proportions inject: 0 into: [:sum :p | sum + p].
(1 to: size) collect: [:i | | x begin value done |
	x := i - 1.
	begin := 0.
	value := points last.
	done := false.
	1 to: proportions size do: [:k | | length |
		length := (proportions at: k) / total * size.
		(done not and: [x < (begin + length)]) ifTrue: [
			value := (points at: k) + (((points at: k + 1) - (points at: k)) * (x - begin / length)).
			done := true].
		begin := begin + length].
	value])"
            : R"("Exponential curves from each end point to the next, each taking its
proportion of the wavetable; the end points lie above 0."
| points proportions total |
points := #(0.001 1 0.001).
proportions := #(0.1 0.9).
total := proportions inject: 0 into: [:sum :p | sum + p].
(1 to: size) collect: [:i | | x begin value done |
	x := i - 1.
	begin := 0.
	value := points last.
	done := false.
	1 to: proportions size do: [:k | | length |
		length := (proportions at: k) / total * size.
		(done not and: [x < (begin + length)]) ifTrue: [
			value := (points at: k) * ((points at: k + 1) / (points at: k) raisedTo: x - begin / length).
			done := true].
		begin := begin + length].
	value])";
    case SampleTemplateProgram::CubicSpline:
        return R"("A smooth curve through the points, a natural cubic spline, each point
its proportion of the wavetable after the one before."
| points proportions total n xs second u |
points := #(0 1 0 -1 0).
proportions := #(0.25 0.25 0.25 0.25).
total := proportions inject: 0 into: [:sum :p | sum + p].
n := points size.
xs := OrderedCollection new.
second := OrderedCollection new.
u := OrderedCollection new.
xs add: 0.
1 to: n - 1 do: [:k | xs add: (xs at: k) + ((proportions at: k) / total)].
1 to: n do: [:k | second add: 0. u add: 0].
2 to: n - 1 do: [:i | | sig p slope |
	sig := (xs at: i) - (xs at: i - 1) / ((xs at: i + 1) - (xs at: i - 1)).
	p := sig * (second at: i - 1) + 2.
	second at: i put: sig - 1 / p.
	slope := (points at: i + 1) - (points at: i) / ((xs at: i + 1) - (xs at: i))
		- ((points at: i) - (points at: i - 1) / ((xs at: i) - (xs at: i - 1))).
	u at: i put: (6 * slope / ((xs at: i + 1) - (xs at: i - 1)) - (sig * (u at: i - 1))) / p].
second at: n put: 0.
n - 1 to: 1 by: -1 do: [:i | second at: i put: (second at: i) * (second at: i + 1) + (u at: i)].
(1 to: size) collect: [:j | | x k h a b |
	x := size > 1 ifTrue: [j - 1 / (size - 1)] ifFalse: [0].
	k := 1.
	[k + 1 < n and: [x > (xs at: k + 1)]] whileTrue: [k := k + 1].
	h := (xs at: k + 1) - (xs at: k).
	a := (xs at: k + 1) - x / h.
	b := x - (xs at: k) / h.
	a * (points at: k) + (b * (points at: k + 1))
		+ ((a * a * a - a) * (second at: k) + ((b * b * b - b) * (second at: k + 1)) * h * h / 6)])";
    case SampleTemplateProgram::ImpulseResponse:
        return R"("A filter's response to an impulse, at 48000 samples a second: each
formant a sine at its centre frequency, decaying as its bandwidth says."
| frequencies bandwidths amplitudes gain rate |
frequencies := #(500 1500 2500).
bandwidths := #(80 120 160).
amplitudes := #(1 0.5 0.25).
gain := 0.1.
rate := 48000.
(1 to: size) collect: [:i | | t |
	t := i - 1 / rate.
	(1 to: frequencies size) inject: 0 into: [:sum :k |
		sum + (gain * (amplitudes at: k) * (Float pi negated * (bandwidths at: k) * t) exp
			* (2 * Float pi * (frequencies at: k) * t) sin)]])";
    }
    return {};
}

SampleTemplateProgramResult runSampleTemplateProgram(std::string_view source, std::size_t size)
{
    if (size == 0)
        return {{}, "A wavetable needs at least one sample"};
    const auto parsed = smalltalk::parse(source);
    if (!parsed.ok())
        return {{}, parsed.diagnostic->message};
    ProgramHost host{size};
    const auto ran = smalltalk::run(*parsed.program, host);
    if (!ran.ok())
        return {{}, ran.diagnostic->message};
    if (ran.value.kind != smalltalk::Value::Kind::Array || ran.value.elements == nullptr
        || ran.value.elements->size() != size)
        return {{}, "The program should answer an Array of " + std::to_string(size) + " numbers"};
    SampleTemplateProgramResult result;
    result.samples.reserve(size);
    for (const auto& element : *ran.value.elements)
    {
        if (element.kind != smalltalk::Value::Kind::Number || !std::isfinite(element.number))
            return {{}, "Each of the program's samples should be a number"};
        result.samples.push_back(element.number);
    }
    const auto peak = std::ranges::max(result.samples, {}, [](double value) { return std::abs(value); });
    if (std::abs(peak) > 1.0)
        for (auto& sample : result.samples)
            sample /= std::abs(peak);
    return result;
}
}
