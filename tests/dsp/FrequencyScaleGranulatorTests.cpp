#include "talas/dsp/FrequencyScaleGranulator.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
using Granulator = talas::dsp::FrequencyScaleGranulator;

constexpr double sampleRate = 48000.0;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Ten harmonics of 200 hz, the nth at 0.1 / n.
std::vector<double> harmonicTone(std::size_t frames)
{
    std::vector<double> tone(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (int harmonic = 1; harmonic <= 10; ++harmonic)
            tone[frame] += 0.1 / harmonic
                * std::sin(2.0 * std::numbers::pi * 200.0 * harmonic * static_cast<double>(frame) / sampleRate);
    return tone;
}

// The level of `frequency` in a Hann-windowed stretch of `signal`.
double level(const std::vector<double>& signal, std::size_t from, std::size_t count, double frequency)
{
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t n = 0; n < count; ++n)
    {
        const auto window = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(n) / static_cast<double>(count));
        const auto angle = 2.0 * std::numbers::pi * frequency * static_cast<double>(n) / sampleRate;
        real += window * signal[from + n] * std::cos(angle);
        imaginary += window * signal[from + n] * std::sin(angle);
    }
    return 4.0 * std::hypot(real, imaginary) / static_cast<double>(count);
}

std::vector<double> granulate(Granulator& granulator, const std::vector<double>& input, double frequency, double scale)
{
    std::vector<double> output(input.size());
    const std::vector<double> tracked(input.size(), frequency);
    const std::vector<double> scales(input.size(), scale);
    for (std::size_t block = 0; block < input.size(); block += 64)
    {
        const auto count = std::min<std::size_t>(64, input.size() - block);
        require(granulator.process(std::span{input}.subspan(block, count), std::span{tracked}.subspan(block, count),
                    std::span{scales}.subspan(block, count), std::span{output}.subspan(block, count))
                == Granulator::Status::Ok,
            "the granulator should render");
    }
    return output;
}

// Grains of the tracked period overlap to scale the frequency up and
// leave time between them to scale it down, each keeping the Input's
// spectrum where it is.
void testScalesTheTrackedFrequency()
{
    const auto tone = harmonicTone(48000);
    for (const auto scale : {2.0, 1.5, 0.5})
    {
        Granulator granulator;
        require(granulator.prepare({sampleRate, 1024, 2.0, {}}), "the granulator should prepare");
        const auto output = granulate(granulator, tone, 200.0, scale);
        const auto fundamental = 200.0 * scale;
        require(level(output, 24000, 9600, fundamental) > 0.02, "the scaled fundamental should sound");
        if (scale > 1.0)
            require(level(output, 24000, 9600, 200.0) < 1.0e-3, "the Input's fundamental should be gone");
    }
    Granulator octave;
    require(octave.prepare({sampleRate, 1024, 2.0, {}}), "the granulator should prepare");
    const auto up = granulate(octave, tone, 200.0, 2.0);
    // The second harmonic keeps its level as the new fundamental.
    require(std::abs(level(up, 24000, 9600, 400.0) - 0.05) < 0.005, "an octave up should keep the spectrum's level");
}

// A scale of 1 lays the grains a period apart and gives back the Input,
// Delay frames and one to two periods late.
void testScaleOfOneGivesBackTheInput()
{
    const auto tone = harmonicTone(24000);
    Granulator granulator;
    require(granulator.prepare({sampleRate, 512, 1.0, {}}), "the granulator should prepare");
    const auto output = granulate(granulator, tone, 200.0, 1.0);
    for (std::size_t frame = 0; frame < 512; ++frame)
        require(output[frame] == 0.0, "the output should wait for the Delay");
    auto best = 1.0;
    for (std::size_t lag = 512 + 240; lag < 512 + 480; ++lag)
    {
        auto worst = 0.0;
        for (std::size_t frame = 12000; frame < 24000; ++frame)
            worst = std::max(worst, std::abs(output[frame] - tone[frame - lag]));
        best = std::min(best, worst);
    }
    require(best < 1.0e-3, "a scale of 1 should give back the Input a Delay and a period or two late");
}

// The Window shapes each grain: a rectangle across both periods doubles the
// level of overlapping Hann grains.
void testWindowShapesEachGrain()
{
    const auto tone = harmonicTone(24000);
    Granulator granulator;
    require(granulator.prepare({sampleRate, 0, 1.0, std::vector<double>(64, 1.0)}), "the granulator should prepare");
    const auto output = granulate(granulator, tone, 200.0, 1.0);
    require(std::abs(level(output, 12000, 9600, 200.0) - 0.2) < 0.01, "rectangular grains should overlap at twice the level");
}

// A copy carries on exactly as the original, and rendering allocates nothing.
void testCopiedStateCarriesOnWithoutAllocating()
{
    const auto tone = harmonicTone(4096);
    Granulator first;
    Granulator second;
    require(first.prepare({sampleRate, 256, 3.0, {}}) && second.prepare({sampleRate, 256, 3.0, {}}),
        "the granulators should prepare");
    std::vector<double> frequency(tone.size(), 310.0);
    std::vector<double> scale(tone.size());
    for (std::size_t frame = 0; frame < scale.size(); ++frame)
        scale[frame] = 0.5 + 2.5 * static_cast<double>(frame) / static_cast<double>(scale.size());
    std::vector<double> a(tone.size());
    std::vector<double> b(tone.size());
    const auto half = tone.size() / 2;
    allocationCount = 0;
    countAllocations = true;
    const auto rendered = first.process(std::span{tone}.first(half), std::span{frequency}.first(half),
        std::span{scale}.first(half), std::span{a}.first(half));
    const auto copied = second.copyStateFrom(first);
    const auto restA = first.process(std::span{tone}.subspan(half), std::span{frequency}.subspan(half),
        std::span{scale}.subspan(half), std::span{a}.subspan(half));
    const auto restB = second.process(std::span{tone}.subspan(half), std::span{frequency}.subspan(half),
        std::span{scale}.subspan(half), std::span{b}.subspan(half));
    countAllocations = false;
    require(rendered == Granulator::Status::Ok && copied && restA == Granulator::Status::Ok
            && restB == Granulator::Status::Ok,
        "the granulators should render and copy");
    require(allocationCount == 0, "rendering and copying should not allocate");
    for (std::size_t frame = half; frame < tone.size(); ++frame)
        require(a[frame] == b[frame], "a copied granulator should carry on as the original");

    Granulator other;
    require(other.prepare({sampleRate, 128, 3.0, {}}), "the granulator should prepare");
    require(!other.copyStateFrom(first), "a granulator prepared differently should refuse the copy");
}

void testRefusesWhatItCannotPlay()
{
    Granulator granulator;
    require(!granulator.prepare({0.0, 0, 1.0, {}}), "a granulator needs a sample rate");
    require(!granulator.prepare({sampleRate, 0, 65.0, {}}), "MaxScale is at most 64");
    require(!granulator.prepare({sampleRate, 0, 1.0 / 128.0, {}}), "MaxScale is at least 1/64");
    require(!granulator.prepare({sampleRate, 0, 1.0, {1.0}}), "a Window needs two points");
    require(!granulator.prepare({sampleRate, 0, 1.0, {0.0, NAN}}), "a Window must be finite");
    require(Granulator::supportedScale(3.0) && !Granulator::supportedScale(0.0), "scales are positive");
    std::vector<double> frame{NAN};
    std::vector<double> out(1);
    require(granulator.prepare({sampleRate, 0, 1.0, {}}), "the granulator should prepare");
    require(granulator.process(frame, std::vector<double>{200.0}, std::vector<double>{1.0}, out)
            == Granulator::Status::NonFiniteInput,
        "a non-finite Input should be refused");
}
}

int main()
{
    try
    {
        testScalesTheTrackedFrequency();
        testScaleOfOneGivesBackTheInput();
        testWindowShapesEachGrain();
        testCopiedStateCarriesOnWithoutAllocating();
        testRefusesWhatItCannotPlay();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "frequency scale granulator tests passed\n";
    return 0;
}
