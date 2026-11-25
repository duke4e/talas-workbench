#include "talas/dsp/PreparedSpectrumFilterBank.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <ranges>
#include <span>
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

namespace
{
constexpr double sampleRate = 48000.0;
// The tests below were written for the hertz Bandwidth of projects before
// format 58.
constexpr auto legacyHertz = talas::dsp::SpectrumFilterBandwidthUnits::LegacyHertz;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

[[nodiscard]] std::vector<double> tone(
    double frequency,
    std::size_t frames)
{
    std::vector<double> result(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        result[frame] = std::sin(
            2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate);
    }
    return result;
}

[[nodiscard]] double projectedAmplitude(
    std::span<const double> samples,
    double frequency)
{
    const auto first = samples.size() / 2;
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t frame = first; frame < samples.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sine += samples[frame] * std::sin(phase);
        cosine += samples[frame] * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine)
        / static_cast<double>(samples.size() - first);
}

void testIsolatedBandsAndAmplitudeScaling()
{
    constexpr std::size_t frames = 16384;
    auto input = tone(1000.0, frames);
    std::vector<double> output(frames);
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 2> bands{{
        {0.5, 1000.0, 200.0},
        {1.0, 4000.0, 300.0},
    }};
    talas::dsp::PreparedSpectrumFilterBank bank;
    require(bank.configure(sampleRate, bands, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "valid mapped filter bank should configure");
    require(bank.process(input, output, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "mapped filter bank should process");
    const auto matching = projectedAmplitude(output, 1000.0);
    const auto rejected = projectedAmplitude(output, 4000.0);
    require(matching > 0.45 && matching < 0.55,
        "matching isolated band should preserve the authored amplitude");
    require(rejected < matching * 0.01,
        "non-present isolated band should remain rejected");
}

void testPartitionIdentityAndConnectedBandwidth()
{
    constexpr std::size_t frames = 4096;
    const auto input = tone(1000.0, frames);
    std::vector<double> wholeOutput(frames);
    std::vector<double> partitionedOutput(frames);
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1> band{{
        {1.0, 1000.0, 100.0},
    }};
    talas::dsp::PreparedSpectrumFilterBank whole;
    talas::dsp::PreparedSpectrumFilterBank partitioned;
    require(whole.configure(sampleRate, band, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && partitioned.configure(sampleRate, band, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "partition fixtures should configure");
    require(whole.process(input, wholeOutput, 250.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "whole override render should process");
    constexpr std::array<std::size_t, 4> chunks{17, 255, 3, 511};
    std::size_t offset = 0;
    std::size_t chunk = 0;
    while (offset < frames)
    {
        const auto count = std::min(chunks[chunk++ % chunks.size()],
            frames - offset);
        require(partitioned.configure(sampleRate, band, legacyHertz)
                    == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
            "identical configuration should preserve state");
        require(partitioned.process(
                    std::span<const double>{input}.subspan(offset, count),
                    std::span<double>{partitionedOutput}.subspan(offset, count),
                    250.0)
                    == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
            "partitioned override render should process");
        offset += count;
    }
    require(wholeOutput == partitionedOutput,
        "identical publication should retain exact partition continuity");

    std::vector<double> connected(frames, 250.0);
    std::vector<double> connectedOutput(frames);
    talas::dsp::PreparedSpectrumFilterBank connectedBank;
    require(connectedBank.configure(sampleRate, band, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && connectedBank.process(
                input, connectedOutput, 0.0, connected)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "connected bandwidth frame should process");
    require(connectedOutput == wholeOutput,
        "constant connected bandwidth should match the static override exactly");

    std::vector<double> authoredOutput(frames);
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1>
        authoredBand{{{1.0, 1000.0, 250.0}}};
    talas::dsp::PreparedSpectrumFilterBank authoredBank;
    require(authoredBank.configure(sampleRate, authoredBand, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && authoredBank.process(input, authoredOutput, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "authored bandwidth fixture should process");
    require(authoredOutput == wholeOutput,
        "zero override should select the authored bandwidth exactly");
}

void testParallelSumInactiveBandsAndStatePublication()
{
    constexpr std::size_t frames = 256;
    const auto input = tone(1000.0, frames);
    std::array<double, frames> parallelOutput{};
    std::array<double, frames> singleOutput{};
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 2>
        parallelBands{{
            {0.25, 1000.0, 200.0},
            {0.75, 1000.0, 200.0},
        }};
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1>
        singleBand{{{1.0, 1000.0, 200.0}}};
    talas::dsp::PreparedSpectrumFilterBank parallel;
    talas::dsp::PreparedSpectrumFilterBank single;
    require(parallel.configure(sampleRate, parallelBands, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && single.configure(sampleRate, singleBand, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && parallel.process(input, parallelOutput, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && single.process(input, singleOutput, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "parallel-sum fixture should process");
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        require(std::abs(parallelOutput[frame] - singleOutput[frame]) < 1.0e-15,
            "parallel bands should sum without hidden normalization");
    }

    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1>
        inactive{{{0.0, -1.0, 0.0}}};
    std::array<double, frames> inactiveOutput{};
    talas::dsp::PreparedSpectrumFilterBank inactiveBank;
    require(inactiveBank.configure(sampleRate, inactive, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && inactiveBank.process(input, inactiveOutput, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && std::ranges::all_of(inactiveOutput,
                [](double sample) { return sample == 0.0; }),
        "inactive bands should contribute exact zero");

    std::array<double, frames> impulse{};
    impulse[0] = 1.0;
    std::array<double, frames> history{};
    std::array<double, frames> tail{};
    talas::dsp::PreparedSpectrumFilterBank retained;
    require(retained.configure(sampleRate, singleBand, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && retained.process(impulse, history, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "same-count state fixture should prime");
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1>
        changedGain{{{0.5, 1000.0, 200.0}}};
    std::array<double, frames> silence{};
    require(retained.configure(sampleRate, changedGain, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && retained.process(silence, tail, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && std::ranges::any_of(tail,
                [](double sample) { return sample != 0.0; }),
        "same-count configuration publication should retain indexed state");

    talas::dsp::PreparedSpectrumFilterBank countChanged;
    require(countChanged.configure(sampleRate, parallelBands, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && countChanged.process(impulse, history, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && countChanged.configure(sampleRate, singleBand, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && countChanged.process(silence, tail, 0.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && std::ranges::all_of(tail,
                [](double sample) { return sample == 0.0; }),
        "band-count change should reset all retained state once");
}

void testFailureAtomicRecoveryAndNoAllocation()
{
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1> valid{{
        {1.0, 1000.0, 100.0},
    }};
    constexpr std::array<talas::dsp::SpectrumFilterBandConfiguration, 1> invalid{{
        {1.0, 30000.0, 100.0},
    }};
    std::array<double, 128> input{};
    input.fill(1.0);
    std::array<double, 128> output{};
    talas::dsp::PreparedSpectrumFilterBank bank;
    require(bank.configure(sampleRate, valid, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "recovery fixture should configure");
    require(bank.configure(sampleRate, invalid, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::InvalidConfiguration
            && !bank.ready(),
        "invalid candidate should fail closed");
    require(bank.configure(sampleRate, valid, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "complete valid candidate should recover");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto configureStatus = bank.configure(sampleRate, valid, legacyHertz);
    const auto processStatus = bank.process(input, output, 100.0);
    bank.reset();
    countAllocations.store(false, std::memory_order_release);
    require(configureStatus == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && processStatus
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "configure/process/reset should not allocate");

    std::array<double, 128> malformedBandwidth{};
    malformedBandwidth.fill(100.0);
    malformedBandwidth[64] = -1.0;
    require(bank.configure(sampleRate, valid, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && bank.process(input, output, 0.0, malformedBandwidth)
                == talas::dsp::PreparedSpectrumFilterBankStatus::InvalidConfiguration,
        "malformed connected bandwidth should reject before processing");
    require(std::ranges::all_of(output,
                [](double sample) { return sample == 0.0; }),
        "malformed connected bandwidth should clear the complete output");

    input[17] = std::numeric_limits<double>::infinity();
    require(bank.configure(sampleRate, valid, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok
            && bank.process(input, output, 100.0)
                == talas::dsp::PreparedSpectrumFilterBankStatus::NumericFailure
            && std::ranges::all_of(output,
                [](double sample) { return sample == 0.0; }),
        "non-finite input should fail closed and clear complete output");
}

// The Bandwidth is a 0 to 1 control: each filter is that fraction
// of its spacing from the nearer neighbouring partial wide, and a filter of
// no width passes nothing.
void testBandwidthScalesTheSpacing()
{
    require(talas::dsp::spectrumFilterSpacing(900.0, 1000.0, 1500.0) == 100.0
            && talas::dsp::spectrumFilterSpacing(
                   std::numeric_limits<double>::quiet_NaN(), 1000.0, 1500.0) == 500.0
            && talas::dsp::spectrumFilterSpacing(
                   std::numeric_limits<double>::quiet_NaN(), 1000.0,
                   std::numeric_limits<double>::quiet_NaN()) == 1000.0
            && talas::dsp::spectrumFilterSpacing(1000.0, 1000.0, 1200.0) == 200.0,
        "a filter's spacing is the distance to its nearer distinct neighbour, or its center alone");

    constexpr std::size_t frames = 48000;
    constexpr auto spacing = talas::dsp::SpectrumFilterBandwidthUnits::Spacing;
    // A 1000 hz filter 200 hz from its neighbour, at Bandwidth 0.5, is 100 hz
    // wide: half power at its band edges.
    const std::array<talas::dsp::SpectrumFilterBandConfiguration, 1> band{{{1.0, 1000.0, 200.0}}};
    const auto q = 1000.0 / 100.0;
    const auto edge = 1000.0 * (std::sqrt(1.0 + 1.0 / (4.0 * q * q)) + 1.0 / (2.0 * q));
    const auto gainAt = [&](double frequency, double bandwidth) {
        talas::dsp::PreparedSpectrumFilterBank bank;
        require(bank.configure(sampleRate, band, spacing)
                    == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
            "a spacing filter bank should configure");
        const auto input = tone(frequency, frames);
        std::vector<double> output(frames);
        require(bank.process(input, output, bandwidth)
                    == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
            "a spacing filter bank should process");
        return projectedAmplitude(output, frequency);
    };
    require(std::abs(gainAt(1000.0, 0.5) - 1.0) < 1.0e-3,
        "a spacing filter should pass its center");
    require(std::abs(gainAt(edge, 0.5) - std::sqrt(0.5)) < 0.01,
        "Bandwidth 0.5 should make the filter half its spacing wide");
    require(gainAt(1000.0, 0.0) == 0.0,
        "Bandwidth 0 should pass nothing");
}

void testMaximumBandCallbackBudget()
{
    constexpr std::size_t frames = 256;
    constexpr std::size_t iterations = 400;
    std::array<talas::dsp::SpectrumFilterBandConfiguration,
        talas::dsp::maximumPreparedSpectrumFilterBandCount> bands{};
    for (std::size_t band = 0; band < bands.size(); ++band)
    {
        bands[band] = {
            1.0 / static_cast<double>(bands.size()),
            100.0 + 100.0 * static_cast<double>(band),
            50.0,
        };
    }
    const auto input = tone(1000.0, frames);
    std::array<double, frames> output{};
    talas::dsp::PreparedSpectrumFilterBank bank;
    require(bank.configure(sampleRate, bands, legacyHertz)
                == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
        "maximum-band timing fixture should configure");
    std::array<std::chrono::nanoseconds, iterations> timings{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto status = bank.process(input, output, 0.0);
        const auto finished = std::chrono::steady_clock::now();
        require(status == talas::dsp::PreparedSpectrumFilterBankStatus::Ok,
            "maximum-band timing fixture should process");
        timings[iteration] = finished - started;
    }
    countAllocations.store(false, std::memory_order_release);
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "maximum-band callback processing should not allocate");
    std::ranges::sort(timings);
    const auto p99 = timings[(iterations * 99) / 100].count();
    std::cout << "spectrum-filter-bank 64-band 256-frame p99 "
              << p99 << " ns\n";
    if (!sanitizerInstrumented)
    {
        require(p99 < 5'333'333,
            "local 64-band FilterBank p99 should fit one callback period");
    }
}
}

int main()
{
    testIsolatedBandsAndAmplitudeScaling();
    testPartitionIdentityAndConnectedBandwidth();
    testParallelSumInactiveBandsAndStatePublication();
    testFailureAtomicRecoveryAndNoAllocation();
    testBandwidthScalesTheSpacing();
    testMaximumBandCallbackBudget();
    std::cout << "Prepared Spectrum filter bank tests passed\n";
}
