#include "talas/dsp/PreparedTimeFrequencyScale.h"
#include "../support/TimeFrequencyScaleLowOutputReference.h"
#include "../support/TimeFrequencyScaleLowPartialsReference.h"
#include "../support/TimeFrequencyScaleDensePartialsReference.h"
#include "../support/TimeFrequencyScaleVariableSourceReference.h"
#include "../support/TestShard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocations = 0;
}
void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed)) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace talas::dsp
{
struct PreparedTimeFrequencyScaleTestAccess
{
    // Scheduling isolation only: bypass discovery so every window/capacity can
    // exercise activation and reference FFT work. Separate source-only and
    // end-to-end tests must discover their components through the real model.
    static void seedCandidate(PreparedTimeFrequencyScale& value, std::span<const double> frequencies)
    {
        auto& candidate = value.components_.candidate;
        candidate = {}; candidate.present = true; candidate.count = frequencies.size(); candidate.dc = .1;
        for (std::size_t k = 0; k < frequencies.size(); ++k)
        {
            candidate.frequency[k] = frequencies[k]; candidate.amplitude[k] = .2/(k+1);
            candidate.phase[k] = .37-.63*k;
        }
        value.components_.candidateReady = true;
    }
    static bool startAnalysis(PreparedTimeFrequencyScale& value, std::span<const double> source)
    { return value.componentAnalysis_->begin(source.first(value.frameSize_), value.sampleRate_); }
    static std::uint64_t analysisWork(const PreparedTimeFrequencyScale& value)
    { return value.componentAnalysis_->consumedWork(); }
};
}

namespace
{
constexpr double sampleRate = 48000;
constexpr double tau = 2 * std::numbers::pi;
using Processor = talas::dsp::PreparedTimeFrequencyScale;
using Status = talas::dsp::PhaseVocoderPitchStatus;
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
std::vector<double> tone(double frequency, double dc = 0.0, std::size_t length = 48000)
{
    std::vector<double> result(length);
    for (std::size_t i = 0; i < length; ++i)
        result[i] = dc + 0.25 * std::sin(tau * frequency * i / sampleRate);
    return result;
}
double magnitude(std::span<const double> values, double frequency)
{
    std::complex<double> projection{};
    for (std::size_t i = 0; i < values.size(); ++i)
        projection += values[i] * (0.5 - 0.5 * std::cos(tau * i / values.size()))
            * std::polar(1.0, -tau * frequency * i / sampleRate);
    return std::abs(projection) / values.size();
}
void process(Processor& processor, std::span<const double> source, double scale, double rate,
    std::span<double> output, std::uint64_t start = 0, std::size_t partition = 128,
    double* maximumCallbackMicroseconds = nullptr)
{
    for (std::size_t offset = 0; offset < output.size(); offset += partition)
    {
        allocations = 0;
        countAllocations = true;
        const auto begin = maximumCallbackMicroseconds != nullptr
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto status = processor.processSource(source, scale, rate, {5, 7, start + offset}, 0,
            output.subspan(offset, std::min(partition, output.size() - offset)));
        if (maximumCallbackMicroseconds != nullptr)
            *maximumCallbackMicroseconds = std::max(*maximumCallbackMicroseconds,
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count());
        countAllocations = false;
        require(status == Status::Ok, "source processing should succeed");
        require(allocations == 0, "source processing must allocate nothing");
    }
}
void equal(std::span<const double> first, std::span<const double> second)
{
    require(first.size() == second.size(), "output sizes");
    for (std::size_t i = 0; i < first.size(); ++i)
        require(std::isfinite(first[i]) && std::abs(first[i] - second[i]) < 1e-12,
            "partition/reset/checkpoint must retain exact source and spectral state");
}
void testLongFftAdmissionAndRoundTrip()
{
    using namespace talas::dsp;
    require(!isSupportedRadix2FftSize(8192), "default FFT profile must stay bounded");
    require(!isSupportedRadix2FftSize(65537, Radix2FftSizeProfile::LongAnalysis)
        && !isSupportedRadix2FftSize(131072, Radix2FftSizeProfile::LongAnalysis)
        && !isSupportedRadix2FftSize(1024, static_cast<Radix2FftSizeProfile>(99)), "closed FFT bounds");
    bool rejected = false;
    try { Radix2FftPlan invalid{8192}; } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "constructor default FFT bound");
    for (const auto size : {std::size_t{8192}, std::size_t{65536}})
    {
        Radix2FftPlan fft{size, Radix2FftSizeProfile::LongAnalysis};
        std::vector<std::complex<double>> signal(size);
        for (std::size_t i = 0; i < size; ++i) signal[i] = std::polar(0.25, tau * 137 * i / size);
        const auto original = signal;
        allocations = 0;
        countAllocations = true;
        const auto forward = fft.forward(signal);
        countAllocations = false;
        require(forward && allocations == 0, "long FFT noalloc");
        require(std::abs(signal[137] - std::complex<double>{0.25 * size, 0}) < 1e-7,
            "long FFT independently known complex bin");
        require(fft.inverse(signal), "long inverse");
        for (std::size_t i = 0; i < size; ++i)
            require(std::abs(signal[i] - original[i]) < 1e-12, "long FFT roundtrip");
    }
    require(timeFrequencyScaleAnalysisFrameSize(48000, 20) == 8192
        && timeFrequencyScaleAnalysisFrameSize(48000, 40) == 4096
        && timeFrequencyScaleAnalysisFrameSize(8192, 20) == 1024
        && timeFrequencyScaleAnalysisFrameSize(48000, 0.1) == 0,
        "source hint controls bounded prepared window");
}
template <typename Value>
void testLongFftMatchesStandardProfile()
{
    using namespace talas::dsp;
    const auto tolerance = std::is_same_v<Value, double> ? 1e-10 : 1e-4;
    for (const auto size : {std::size_t{16}, std::size_t{64}, std::size_t{1024}, std::size_t{4096}})
    {
        Radix2FftPlan standard(size), longAnalysis(size, Radix2FftSizeProfile::LongAnalysis);
        std::vector<std::complex<Value>> source(size);
        std::uint32_t state = 0x27a61c35;
        for (auto& value : source)
        {
            state = state * 1664525U + 1013904223U;
            const auto real = static_cast<Value>((static_cast<double>(state) / 4294967296.0) - 0.5);
            state = state * 1664525U + 1013904223U;
            const auto imag = static_cast<Value>((static_cast<double>(state) / 4294967296.0) - 0.5);
            value = {real, imag};
        }
        auto expected = source, actual = source;
        require(standard.forward(expected) && longAnalysis.forward(actual), "both FFT profiles must transform");
        for (std::size_t i = 0; i < size; ++i)
            require(std::abs(actual[i] - expected[i]) < tolerance,
                "long-analysis butterflies must preserve the standard finite spectrum");
        if (size <= 64)
        {
            for (std::size_t bin = 0; bin < size; ++bin)
            {
                std::complex<double> direct{};
                for (std::size_t i = 0; i < size; ++i)
                    direct += std::complex<double>{source[i].real(), source[i].imag()}
                        * std::polar(1.0, -tau * bin * i / size);
                require(std::abs(std::complex<double>{actual[bin].real(), actual[bin].imag()} - direct)
                        < tolerance, "long-analysis FFT must match independent direct DFT");
            }
        }
        require(longAnalysis.inverse(actual), "long-analysis inverse must transform");
        for (std::size_t i = 0; i < size; ++i)
            require(std::abs(actual[i] - source[i]) < tolerance,
                "long-analysis inverse must restore arbitrary complex input");
    }
}
void testNonFiniteAndOverflowRejection()
{
    for (const auto sample : {std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()})
    {
        Processor processor(sampleRate, 128, 8192, 1.0, 0.25);
        std::vector<double> source(16384, sample);
        std::array<double, 128> output{};
        auto status = Status::Ok;
        allocations = 0;
        countAllocations = true;
        for (std::size_t offset = 0; offset < 10240 && status == Status::Ok; offset += output.size())
            status = processor.processSource(source, 1.0, 0.25, {1, 0, offset}, 0, output);
        countAllocations = false;
        require(allocations == 0, "invalid spectral arithmetic must not allocate");
        require(status == (std::isfinite(sample) ? Status::NumericOverflow : Status::NonFiniteInput),
            "long-analysis arithmetic must still reject non-finite samples and finite-input overflow");
    }
}
void testLowTonesAndDc()
{
    for (const auto frequency : {20.0, 40.0, 80.0, 160.0, 269.0})
    {
        for (const auto dc : {0.0, 0.1})
        {
            const auto source = tone(frequency, dc);
            Processor processor{sampleRate, 128, 8192, 1.0, 0.25};
            std::vector<double> output(60000);
            process(processor, source, 1, 0.25, output);
            const auto measured = std::span<const double>{output}.subspan(24000, 24000);
            const auto wanted = magnitude(measured, frequency);
            const auto slowed = magnitude(measured, frequency * 0.25);
            const auto mean = std::accumulate(measured.begin(), measured.end(), 0.0) / measured.size();
            std::cout << "tone=" << frequency << " dc=" << dc << " wanted=" << wanted
                << " slowed=" << slowed << " mean=" << mean << '\n';
            require(wanted > 0.045 && wanted > 10 * slowed, "original low source pitch must dominate");
            require(std::abs(mean - dc) < 0.006, "low tones must preserve DC independently");
        }
    }
}
void testIndependentPitchAndTwoPartials()
{
    auto source = tone(80, 0.1);
    for (std::size_t i = 0; i < source.size(); ++i)
        source[i] += 0.1 * std::sin(tau * 233 * i / sampleRate);
    for (const auto profile : {std::array{0.5, 0.5}, std::array{0.5, 1.0},
            std::array{2.0, 1.0}, std::array{1.5, 0.5}, std::array{4.0, 1.0}})
    {
        Processor processor{sampleRate, 128, 8192, profile[0], profile[1]};
        std::vector<double> output(48000);
        process(processor, source, profile[0], profile[1], output);
        const auto measured = std::span<const double>{output}.subspan(20000, 20000);
        require(magnitude(measured, 80 * profile[0]) > 0.04
            && magnitude(measured, 233 * profile[0]) > 0.012,
            "independent frequency scale must retain both partials");
    }
}
void testPartitionResetCheckpointAndNeutral()
{
    auto source = tone(40, 0.1, 12000);
    Processor reference{sampleRate, 128, 8192, 1.0, 0.25};
    Processor partitioned{sampleRate, 128, 8192, 1.0, 0.25};
    // The centered analysis support extends by N/2 source samples at EOF;
    // stretch that support by Rate, then flush the N/2 synthesis support.
    constexpr std::size_t latency = 8192 + 8192 / 4 - 1;
    constexpr std::size_t tailEnd = 48000 + latency + 8192 / 2 * 5;
    std::vector<double> expected(tailEnd + 1024), actual(expected.size());
    process(reference, source, 1, 0.25, expected);
    process(partitioned, source, 1, 0.25, actual, 0, 7);
    equal(actual, expected);
    require(reference.latencyFrames() == latency
        && std::ranges::all_of(std::span{expected}.first(latency), [](double v) { return v == 0; }),
        "prepared latency must be exact");
    require(std::ranges::all_of(std::span{expected}.last(1024), [](double v) { return std::abs(v) < 1e-12; }),
        "source EOF must flush finite spectral tail");
    allocations = 0;
    countAllocations = true;
    partitioned.reset();
    countAllocations = false;
    require(allocations == 0, "reset noalloc");
    process(partitioned, source, 1, 0.25, std::span{actual}.first(12037));
    auto owned = partitioned.cloneProcessingState();
    Processor checkpoint{sampleRate, 128, 8192, 1.0, 0.25};
    allocations = 0;
    countAllocations = true;
    const auto copied = checkpoint.copyProcessingStateFrom(partitioned);
    countAllocations = false;
    require(copied && allocations == 0, "checkpoint copy noalloc");
    Processor incompatible{sampleRate, 128, 4096, 1.0, 0.25};
    require(!incompatible.copyProcessingStateFrom(partitioned), "different window snapshot rejects");
    std::array<double, 4096> first{}, second{}, third{};
    process(partitioned, source, 1, 0.5, first, 12037);
    process(*owned, source, 1, 0.5, second, 12037, 1);
    require(checkpoint.retimeNextBlockAbsoluteFrame(90000), "checkpoint retime");
    process(checkpoint, source, 1, 0.5, third, 90000, 7);
    equal(first, second);
    equal(first, third);
    Processor neutral{sampleRate, 128, 8192, 0.5, 0.5};
    std::vector<double> delayed(20000);
    process(neutral, source, 0.5, 0.5, delayed);
    for (std::size_t i = latency; i < delayed.size(); ++i)
    {
        const auto position = 0.5 * (i - latency);
        const auto lower = static_cast<std::size_t>(position);
        const auto expectedValue = source[lower] * (1 - (position - lower))
            + source[std::min(lower + 1, source.size() - 1)] * (position - lower);
        require(delayed[i] == expectedValue, "neutral connected profile must be exact delayed interpolation");
    }
}

void testIncrementalFftAndPendingState()
{
    using namespace talas::dsp;
    for (const auto size : {std::size_t{16}, std::size_t{1024}, std::size_t{8192}, std::size_t{65536}})
    {
        Radix2FftPlan plan(size, Radix2FftSizeProfile::LongAnalysis);
        std::vector<std::complex<double>> input(size);
        for (std::size_t i = 0; i < size; ++i)
            input[i] = std::polar(0.25, tau * 137 * i / size) + std::polar(0.125, -tau * 31 * i / size);
        auto expected = input;
        require(plan.forward(expected), "whole FFT reference");
        for (const auto partition : {std::size_t{1}, std::size_t{7}, std::size_t{257}})
        {
            auto actual = input;
            auto cursor = plan.beginTransform(false);
            std::size_t none = 0;
            require(plan.advanceTransform(actual, cursor, none) && actual == input && !cursor.complete(),
                "zero FFT budget must preserve unfinished input");
            allocations = 0;
            countAllocations = true;
            bool success = true;
            std::size_t totalWork = 0;
            while (!cursor.complete() && success)
            {
                auto work = partition;
                success = plan.advanceTransform(actual, cursor, work);
                totalWork += partition - work;
            }
            countAllocations = false;
            const auto expectedWork = size + size / 2 * static_cast<std::size_t>(std::log2(size));
            require(success && allocations == 0 && totalWork == expectedWork,
                "incremental FFT must consume exactly its bounded work without allocation");
            for (std::size_t i = 0; i < size; ++i)
                require(std::abs(actual[i] - expected[i]) < 1e-8, "incremental FFT preserves every complex bin");
            cursor = plan.beginTransform(true);
            std::size_t prefix = size + size / 2 + 3;
            require(plan.advanceTransform(actual, cursor, prefix) && !cursor.complete(), "inverse must pause mid-transform");
            auto checkpoint = actual;
            auto copiedCursor = cursor;
            while (!cursor.complete())
            {
                std::size_t work = 257;
                require(plan.advanceTransform(actual, cursor, work), "inverse continuation");
            }
            while (!copiedCursor.complete())
            {
                std::size_t work = 31;
                require(plan.advanceTransform(checkpoint, copiedCursor, work), "inverse checkpoint continuation");
            }
            for (std::size_t i = 0; i < size; ++i)
                require(std::abs(actual[i] - input[i]) < 1e-12 && std::abs(checkpoint[i] - actual[i]) < 1e-12,
                    "inverse checkpoint must restore complex samples across work partitions");
        }
        auto cursor = plan.beginTransform(false);
        std::size_t work = 17;
        require(!plan.advanceTransform(std::span(input).first(size - 1), cursor, work) && work == 17,
            "incremental FFT must reject mismatched storage without spending work");
    }

    constexpr std::size_t window = 8192, hop = window / 4, prefix = 2 * window - 1;
    const auto source = tone(40, 0.1, 96000);
    Processor whole(sampleRate, 128, window, 1, 2), candidate(sampleRate, 128, window, 1, 2);
    Processor checkpoint(sampleRate, 128, window, 1, 2);
    std::vector<double> expected(4 * window), discard(prefix + hop), actual(hop + 128);
    process(whole, source, 1, 2, expected);
    process(candidate, source, 1, 2, std::span(discard).first(prefix));
    std::size_t committed = prefix;
    for (std::size_t offset = 1; offset < hop; offset += 31)
    {
        process(candidate, source, 1, 2, std::span(discard).first(prefix + offset - committed), committed);
        committed = prefix + offset;
        allocations = 0;
        countAllocations = true;
        const auto copied = checkpoint.copyProcessingStateFrom(candidate);
        countAllocations = false;
        require(copied && allocations == 0, "pending spectral work must checkpoint without allocation");
        require(checkpoint.retimeNextBlockAbsoluteFrame(90000), "pending frame retime");
        process(checkpoint, source, 1, 2, actual, 90000, 7);
        equal(actual, std::span(expected).subspan(committed, actual.size()));
        if (offset == 1)
        {
            auto owned = candidate.cloneProcessingState();
            process(*owned, source, 1, 2, actual, committed, 1);
            equal(actual, std::span(expected).subspan(committed, actual.size()));
        }
    }
}

void testPendingFrameRetainsItsLiveCapture()
{
    constexpr std::size_t window = 8192, hop = window / 4;
    // Reach a fully populated source window; the first frame at Rate.5 starts
    // with zero source support and cannot provide an audible capture oracle.
    constexpr std::size_t capturePrefix = 3 * window;
    auto changing = tone(40, 0.1, 48000);
    const auto original = changing;
    Processor reference(sampleRate, 128, window, 1, 0.5), candidate(sampleRate, 128, window, 1, 0.5);
    std::vector<double> expected(capturePrefix + 2 * hop - 1), actual(expected.size());
    process(reference, original, 1, 0.5, expected);
    process(candidate, changing, 1, 0.5, std::span(actual).first(capturePrefix));
    for (auto& sample : changing) sample = -sample;
    process(candidate, changing, 1, 0.5, std::span(actual).subspan(capturePrefix), capturePrefix, 1);
    equal(actual, expected);
    require(std::ranges::any_of(std::span(actual).last(hop), [](double sample) { return std::abs(sample) > 1e-5; }),
        "captured frame must have audible output despite later source replacement");
}

void testResetDiscardsPendingAndWrappedState()
{
    const auto source = tone(40, 0.1, 1048576);
    const auto dirtySource = tone(160, -0.2, source.size());
    const std::vector<double> silence(source.size());
    constexpr std::array<std::pair<double, double>, 3> profiles{{{1, 0.25}, {0.5, 0.5}, {1, 2}}};
    for (std::size_t window = 1024; window <= 65536; window *= 2)
    {
        const auto hop = window / 4, latency = window + hop - 1;
        for (const auto [scale, rate] : profiles)
        {
            Processor fresh(sampleRate, 128, window, scale, rate);
            Processor candidate(sampleRate, 128, window, scale, rate);
            Processor checkpoint(sampleRate, 128, window, scale, rate);
            std::vector<double> expected(latency + 2 * window), actual(expected.size());
            std::vector<double> discard(4 * window + hop);
            process(fresh, source, scale, rate, expected);
            // Exercise pre-capture history, the first job, and different stages
            // of later auxiliary-transform jobs after the overlap ring wraps.
            const std::array dirtyLengths{std::size_t{1}, window - 1, window + hop / 2,
                4 * window, 4 * window + hop / 4, 4 * window + hop / 2,
                4 * window + hop * 3 / 4, 4 * window + hop - 1};
            for (const auto dirtyLength : dirtyLengths)
            {
                candidate.reset();
                process(candidate, dirtySource, 2, 2, std::span(discard).first(dirtyLength));
                allocations = 0;
                countAllocations = true;
                candidate.reset();
                candidate.reset();
                const auto copied = checkpoint.copyProcessingStateFrom(candidate);
                countAllocations = false;
                require(copied && allocations == 0, "repeated reset and reset checkpoint must allocate nothing");
                process(candidate, source, scale, rate, actual, 0, 127);
                equal(actual, expected);
                require(std::ranges::all_of(std::span(actual).first(latency), [](double value) { return value == 0; }),
                    "reset must silence stale overlap for the complete latency prefix");
                if (dirtyLength == dirtyLengths.back())
                {
                    // Copy an already-reset instance, then copy its first job
                    // while pending. Both copies must retain fresh OLA behavior.
                    const auto prefix = window;
                    process(checkpoint, source, scale, rate, std::span(actual).first(prefix));
                    require(candidate.copyProcessingStateFrom(checkpoint), "first restarted job checkpoint");
                    process(candidate, source, scale, rate, std::span(actual).subspan(prefix), prefix, 7);
                    equal(actual, expected);
                }
            }
            // Repeated tiny Gate pulses never reach a frame, even after stale
            // audible state has been left in the ring by a complete restart.
            for (int pulse = 0; pulse != 64; ++pulse)
            {
                candidate.reset();
                process(candidate, dirtySource, 2, 2, std::span(actual).first(1));
                require(actual.front() == 0, "one-sample restarts must not leak stale output");
            }
            candidate.reset();
            process(candidate, source, scale, rate, actual);
            equal(actual, expected);
            if (scale == 1 && rate == 0.25)
            {
                // An independent zero-source oracle exposes any old overlap,
                // including the not-yet-consumed ring slot at the first output.
                candidate.reset();
                process(candidate, silence, scale, rate, actual);
                require(std::ranges::all_of(actual, [](double value) { return value == 0; }),
                    "silent source after reset must have no stale spectral tail");
                for (std::size_t offset = 0; offset < hop; offset += std::max(std::size_t{1}, hop / 8))
                {
                    candidate.reset();
                    const auto prefix = window + offset;
                    process(candidate, source, scale, rate, std::span(actual).first(prefix));
                    auto owned = candidate.cloneProcessingState();
                    process(*owned, source, scale, rate, std::span(actual).subspan(prefix), prefix, 7);
                    equal(actual, expected);
                }
            }
        }
    }
}

// Checks the windows from firstWindow to lastWindow frames, doubling. The
// full case list runs each window as its own case, longest first, which
// spreads the longest cases over the four ctest shards (every fourth case).
void testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(
    std::size_t firstWindow = 1024, std::size_t lastWindow = 65536)
{
    std::size_t snapshots = 0;
    std::size_t windows = 0;
    auto source = tone(113, 0.1, 1048576);
    auto dirtySource = source;
    std::uint32_t noise = 0x4a26bf19;
    for (std::size_t i = 0; i < source.size(); ++i)
    {
        noise = noise * 1664525u + 1013904223u;
        source[i] += 0.05 * (static_cast<double>(noise >> 16) / 32768.0 - 1.0)
            + 0.07 * std::sin(tau * 947 * i / sampleRate);
        dirtySource[i] = -0.3 - source[i];
    }
    for (std::size_t window = firstWindow; window <= lastWindow; window *= 2)
    {
        ++windows;
        const auto hop = window / 4;
        for (const auto profile : {0,1,2})
        {
            // Changing rates make every retained clock center distinguishable;
            // the fast sequence also requires auxiliary reference transforms.
            const auto fast = profile != 0;
            const std::array<double, 4> rates = fast
                ? std::array{1.25, 1.5, 1.75, 2.0} : std::array{0.25, 0.5, 0.75, 1.0};
            // Retain both previous scenarios and add variable frequencies with
            // alternating neutral/spectral choices. Future center copies and
            // the compact delayed-choice history must both survive dirty state.
            const std::array scales{.5,1.5,.75,2.0};
            const auto render = [&](Processor& processor, std::span<double> output,
                                    std::size_t start, std::size_t partition) {
                for (std::size_t offset = 0; offset < output.size();)
                {
                    const auto frame = start + offset;
                    const auto count = std::min({output.size() - offset, partition, 257 - frame % 257});
                    const auto control = (frame / 257) % rates.size();
                    process(processor, source, profile == 2 ? scales[control] : 1, rates[control],
                        output.subspan(offset, count), frame, partition);
                    offset += count;
                }
            };
            std::vector<std::size_t> cuts{0, 1, window / 2, window / 2 + 1,
                3 * window / 4, 3 * window / 4 + 1, window - 1};
            // Every sample of the smallest hop exercises even short stages.
            // Larger windows additionally cover both sides of the first wrap.
            for (const auto base : {window, 3 * window})
            {
                const auto step = window == 1024 ? std::size_t{1} : hop / 32;
                for (std::size_t offset = 0; offset < hop; offset += step) cuts.push_back(base + offset);
                cuts.push_back(base + 17 * hop / 32);
                cuts.push_back(base + hop - 1);
                cuts.push_back(base + hop);
            }
            std::ranges::sort(cuts);
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            Processor whole(sampleRate, 128, window, 1, rates[0]);
            Processor candidate(sampleRate, 128, window, 1, rates[0]);
            Processor checkpoint(sampleRate, 128, window, 1, rates[0]);
            std::vector<double> expected(cuts.back() + 2 * window), actual(2 * window), discard(cuts.back());
            render(whole, expected, 0, 128);
            // Seed every destination array with unrelated nonzero audio and
            // controls. Each subsequent continuation leaves it ahead of the
            // next snapshot, exposing reads of accidentally omitted state.
            process(checkpoint, dirtySource, 0.5, 0.5, discard);
            std::size_t committed = 0;
            for (const auto cut : cuts)
            {
                render(candidate, std::span(discard).first(cut - committed), committed, 31);
                committed = cut;
                allocations = 0;
                countAllocations = true;
                const auto copied = checkpoint.copyProcessingStateFrom(candidate);
                countAllocations = false;
                require(copied && allocations == 0, "stage-aware checkpoint must allocate nothing");
                ++snapshots;
                render(checkpoint, actual, cut, 7);
                const auto wanted = std::span(expected).subspan(cut, actual.size());
                equal(actual, wanted);
                require(std::ranges::any_of(wanted, [](double value) { return std::abs(value) > 1e-6; }),
                    "checkpoint comparison must include audible output");
                if (cut == window || cut == 3 * window + 17 * hop / 32)
                {
                    auto owned = candidate.cloneProcessingState();
                    render(*owned, actual, cut, 7);
                    equal(actual, wanted);
                }
            }
        }
    }
    std::cout << "checkpoint_histories windows=" << firstWindow << '-' << lastWindow
        << " scenarios=" << 3 * windows << " variable_frequency_scenarios=" << windows
        << " snapshots=" << snapshots << " exact=1 noalloc=1\n";
}

void testOriginalRateWindowsRespectLiveSourceAvailability()
{
    const auto source = tone(40, 0.1, 16000);
    for (const auto rate : {0.25, 0.5, 1.0})
    {
        const auto scale = rate == 1.0 ? 2.0 : 1.0;
        Processor live{sampleRate, 128, 8192, scale, rate};
        Processor completed{sampleRate, 128, 8192, scale, rate};
        std::vector<double> recording(source.size());
        std::vector<double> actual(24000), expected(actual.size());
        process(completed, source, scale, rate, expected);
        for (std::size_t frame = 0; frame < actual.size(); ++frame)
        {
            if (frame < recording.size()) recording[frame] = source[frame];
            process(live, recording, scale, rate, std::span{actual}.subspan(frame, 1), frame, 1);
        }
        equal(actual, expected);
        require(magnitude(std::span{actual}.subspan(16000, 8000), 40 * scale) > 0.02,
            "live availability evidence must contain the requested low tone");
    }
}

void testFastSourceHopRetainsOffBinPitch()
{
    const auto source = tone(108, 0.1, 262144);
    Processor processor{sampleRate, 128, 8192, 4.0, 8.0};
    std::vector<double> output(48000);
    double maximumCallbackMicroseconds = 0;
    process(processor, source, 4, 8, output, 0, 128, &maximumCallbackMicroseconds);
    const auto measured = std::span{output}.subspan(20000, 12000);
    const auto wanted = magnitude(measured, 432);
    const auto aliased = magnitude(measured, 420.28125);
    std::cout << "Rate8 scale4 wanted=" << wanted << " phase-alias=" << aliased
        << " max kernel callback us=" << maximumCallbackMicroseconds << '\n';
    require(wanted > 0.04 && wanted > 10 * aliased,
        "admitted fast source hops must not alias off-bin source phase");
}

void testChangingRateRetainsIndependentSourceTimeAndPitch()
{
    const talas::test::TimeFrequencyScaleVariableSourceReference reference;
    for (const auto chirp : {false, true})
    {
        const auto source = reference.recording(chirp);
        std::vector<double> output(reference.outputFrames);
        Processor processor(sampleRate, 128, reference.window, 1, reference.changes.front().second);
        std::size_t change = 0;
        for (std::size_t frame = 0; frame < output.size();)
        {
            if (change + 1 < reference.changes.size() && frame == reference.changes[change + 1].first) ++change;
            const auto untilChange = change + 1 < reference.changes.size()
                ? reference.changes[change + 1].first - frame : output.size() - frame;
            const auto frames = std::min({std::size_t{128}, output.size() - frame, untilChange});
            process(processor, source, 1, reference.changes[change].second,
                std::span(output).subspan(frame, frames), frame);
            frame += frames;
        }
        reference.verify(output, chirp, "DSP");
    }
}

void testIndependentFrequencyAndTimeControls()
{
    constexpr std::size_t window = 8192, latency = window + window / 4 - 1;
    const auto source = tone(173, 0.08, 850000);
    const auto measure = [&](std::span<const double> output, double scale, double rate) {
        const auto measured = output.last(24000);
        // A nearly stopped resampling rival approaches DC. Remove only the
        // fixture's known DC offset before comparing AC trajectories; test
        // actual DC preservation separately below.
        std::vector<double> ac(measured.begin(), measured.end());
        for (auto& value : ac) value -= 0.08;
        const auto wanted = magnitude(ac, 173 * scale);
        double wrong = 0, best = 0, peakOffset = 0;
        for (const auto rival : {173.0, 173 * rate})
            if (rival != 173 * scale) wrong = std::max(wrong, magnitude(ac, rival));
        for (double offset = -2; offset <= 2; offset += 0.25)
        {
            const auto value = magnitude(ac, 173 * scale + offset);
            if (value > best) { best = value; peakOffset = offset; }
        }
        const auto mean = std::accumulate(measured.begin(), measured.end(), 0.0) / measured.size();
        const auto valid = wanted > 0.045 && wanted > 10 * wrong
            && std::abs(peakOffset) <= 1 && std::abs(mean - 0.08) < 0.006;
        std::cout << "independent_controls scale=" << scale << " rate=" << rate
            << " wanted=" << wanted << " wrong=" << wrong << " peak_offset_hz=" << peakOffset
            << " mean=" << mean << " pass=" << valid << std::endl;
        return valid;
    };
    std::size_t failures = 0;
    // One common source/window/measurement selects an interior neighborhood
    // even at the slowest rate. No FrequencyScale/Rate quotient is an oracle.
    for (const auto scale : {0.25, 0.5, 1.0, 2.0, 4.0})
        for (const auto rate : {0.125, 0.25, 0.5, 1.0, 2.0, 8.0})
        {
            Processor processor(sampleRate, 128, window, scale, rate);
            std::vector<double> output(latency + 84000);
            process(processor, source, scale, rate, output);
            if (!measure(output, scale, rate)) ++failures;
        }
    // Once the source is away from the leading edge, a tiny positive Rate
    // still requires the original local pitch, even if double clock addition
    // rounds back to the previous position. This is a phase-estimation test.
    for (const auto rate : {1e-5, 1e-12})
    {
        Processor processor(sampleRate, 128, window, 2, 0.5);
        std::vector<double> warmup(64000), output(48000);
        process(processor, source, 2, 0.5, warmup);
        process(processor, source, 2, rate, output, warmup.size());
        if (!measure(output, 2, rate)) ++failures;
    }
    require(failures == 0, "independent pitch/time profiles must retain scaled pitch, gain and DC");

    Processor candidate(sampleRate, 128, window, 2, 0.25);
    Processor pristine(sampleRate, 128, window, 2, 0.25);
    std::array<double, 128> rejected;
    for (const auto invalid : {0.0, -0.25, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()})
        for (const auto invalidScale : {false, true})
        {
            rejected.fill(99);
            const auto scale = invalidScale ? invalid : 2;
            const auto rate = invalidScale ? 0.25 : invalid;
            require(!talas::dsp::isSupportedTimeFrequencyScaleControls(scale, rate),
                "invalid independent control must fail admission");
            bool threw = false;
            try { Processor invalidProcessor(sampleRate, 128, window, scale, rate); }
            catch (const std::invalid_argument&) { threw = true; }
            require(threw, "invalid independent preparation must reject");
            allocations = 0;
            countAllocations = true;
            const auto status = candidate.processSource(source, scale, rate, {5, 7, 0}, 0, rejected);
            countAllocations = false;
            require(status == Status::InvalidInterval && allocations == 0
                && std::ranges::all_of(rejected, [](double value) { return value == 99; }),
                "invalid independent control must reject before output or state mutation");
        }
    std::vector<double> actual(24000), fresh(actual.size());
    process(candidate, source, 2, 0.25, actual);
    process(pristine, source, 2, 0.25, fresh);
    equal(actual, fresh);
}

void testDownwardPitchNearDc()
{
    using Reference = talas::test::TimeFrequencyScaleLowOutputReference;
    std::size_t failures = 0;
    for (const auto frequency : {20.0, 40.0, 80.0})
        for (const auto scale : {0.25, 0.5, 0.125, 0.0625})
            for (const auto dc : {0.0, 0.1})
            {
                const Reference reference{frequency, scale, dc};
                const auto source = reference.recording();
                Processor processor(sampleRate, 128, reference.window, scale, reference.rate);
                std::vector<double> output(reference.outputFrames());
                process(processor, source, scale, reference.rate, output);
                if (!reference.verify(output, "DSP")) ++failures;
            }
    require(failures == 0, "downward pitch near DC must retain tone amplitude, pitch and independent DC");
}

void testMovedNyquistEndpointRetainsAmplitude()
{
    // A Nyquist source has one real endpoint coefficient. Once moved to an
    // interior frequency, its conjugate pair must share that original weight.
    for (const auto scale : {0.5, 0.25, 0.0625})
    {
        const talas::test::TimeFrequencyScaleLowOutputReference reference{24000, scale, 0.1, true};
        const auto source = reference.recording();
        Processor processor(sampleRate, 128, reference.window, scale, reference.rate);
        std::vector<double> output(reference.outputFrames());
        process(processor, source, scale, reference.rate, output);
        require(reference.verify(output, "Nyquist/DSP"),
            "a moved original Nyquist endpoint must preserve tone amplitude and DC");
    }
}

void testLowPartialsRetainIndependentPitchAndGain()
{
    using Reference = talas::test::TimeFrequencyScaleLowPartialsReference;
    // Validate the oracle on ideal close mixtures before asking it to assess
    // production DSP. A stronger neighboring partial must not steal the pitch
    // scan, and individually in-range gains must still retain the gain ratio.
    for (const auto quietFundamental : {false, true})
    {
        const Reference reference{28, 0.0625, 0.75, 0.1, quietFundamental, true};
        const auto frequencies = reference.frequencies(), amplitudes = reference.amplitudes();
        const auto ideal = [&](double upperGain) {
            std::vector<double> output(reference.outputFrames());
            for (std::size_t frame = reference.latencyFrames(); frame < output.size(); ++frame)
            {
                const auto time = static_cast<double>(frame - reference.latencyFrames()) / reference.sampleRate;
                output[frame] = reference.dc
                    + amplitudes[0] * std::sin(tau * frequencies[0] * reference.scale * time + 0.37)
                    + upperGain * amplitudes[1] * std::sin(tau * frequencies[1] * reference.scale * time - 0.91);
            }
            return output;
        };
        require(reference.verify(ideal(1), "oracle/ideal", false),
            "independent mixture oracle must accept exact tones with either amplitude order");
        require(!reference.verify(ideal(1.26), "oracle/ratio-error", false),
            "independent mixture oracle must reject a wrong gain ratio within individual amplitude bounds");
    }
    std::cout << "low_partials analytical oracle: 2 accepted, 2 expected ratio rejections\n";
    std::size_t failures = 0;
    for (const auto upper : {40.0, 28.0})
        for (const auto scale : {0.0625, 0.125})
            for (const auto rate : {0.25, 0.75})
                for (const auto dc : {0.0, 0.1})
                    for (const auto quietFundamental : {false, true})
                        for (const auto shiftedPhases : {false, true})
                        {
                            const Reference reference{upper, scale, rate, dc, quietFundamental, shiftedPhases};
                            const auto source = reference.recording();
                            Processor processor(sampleRate, 128, reference.window, scale, rate);
                            std::vector<double> output(reference.outputFrames());
                            process(processor, source, scale, rate, output);
                            if (!reference.verify(output, "DSP")) ++failures;
                        }
    require(failures == 0, "low partials must retain both scaled pitches, gain ratio and independent DC");
}

void testDensePartialsRetainIndependentPitchAndGain()
{
    using Reference = talas::test::TimeFrequencyScaleDensePartialsReference;
    for (const auto descending : {false, true})
    {
        const Reference reference{16, .0625, .75, descending};
        const auto ideal = reference.ideal();
        const auto measured = std::span(ideal).last(Reference::measurementFrames);
        for (std::size_t k = 0; k < reference.partials; ++k)
        {
            const auto hz = reference.frequency(k)*reference.scale;
            const auto expected = std::polar(reference.amplitude(k),
                tau*hz*3 + reference.phase(k) - std::numbers::pi/2);
            require(std::abs(2.0*Reference::projection(measured, hz)-expected) < 1e-6,
                "independent full-rate projection must retain analytic amplitude and phase");
        }
        require(reference.verify(ideal, false), "dense oracle accepts the exact independent mixture");
        for (const auto defect : {1,2,3,4})
            require(!reference.verify(reference.ideal(defect), false),
                "dense oracle rejects ratio, missing component, Rate pitch and DC counterexamples");
    }
    std::cout << "dense_partials oracle accepted=2 expected_rejections=8\n";
    std::size_t failures = 0, cases = 0;
    for (const auto count : {4U,8U,9U,12U,16U})
        for (const auto scale : {.0625,.125})
            for (const auto rate : {.25,.75})
                for (const auto descending : {false,true})
                {
                    const Reference reference{count, scale, rate, descending};
                    const auto source = reference.recording();
                    Processor processor(sampleRate, 128, reference.window, scale, rate);
                    std::vector<double> output(reference.outputFrames());
                    process(processor, source, scale, rate, output);
                    failures += !reference.verify(output); ++cases;
                }
    std::cout << "dense_partials profiles=" << cases << " passed=" << cases-failures << " failed=" << failures << '\n';
    require(failures == 0, "dense mixtures must retain every scaled pitch, relative gain and DC");
}

void testComponentModelContinuationAndCurrentSource()
{
    const talas::test::TimeFrequencyScaleLowPartialsReference reference{28,.0625,.25,.1,false,true};
    const auto source = reference.recording();
    const auto dirtySource = tone(317, -.2, source.size());
    for (const auto cut : {25000U,60000U,144013U})
    {
        Processor original(sampleRate,128,8192,reference.scale,reference.rate);
        Processor copied(sampleRate,128,8192,reference.scale,reference.rate);
        std::vector<double> prefix(cut), dirty(cut), expected(100000), actual(100000);
        process(original, source, reference.scale, reference.rate, prefix);
        process(copied, dirtySource, reference.scale, reference.rate, dirty);
        if (cut == 25000) require(original.componentAnalysisPending(), "checkpoint exercises pending owned model analysis");
        if (cut == 144013) require(original.componentModelMix() == 1, "checkpoint exercises active component synthesis");
        allocations = 0; countAllocations = true;
        const auto captured = copied.copyProcessingStateFrom(original);
        countAllocations = false;
        require(captured && allocations == 0, "full component job/phase/confidence checkpoint is allocation-free");
        process(original, source, reference.scale, reference.rate, expected, cut);
        process(copied, source, reference.scale, reference.rate, actual, cut, 7);
        require(std::ranges::equal(expected, actual), "pending/active component continuation is bit exact across partitions");
        require(copied.componentModelMix() == 1, "continued source model reaches full confidence");
    }
    Processor changed(sampleRate,128,8192,reference.scale,reference.rate);
    auto mutableSource = source;
    std::vector<double> prefix(25000), silence(180000);
    process(changed, mutableSource, reference.scale, reference.rate, prefix);
    require(changed.componentAnalysisPending(), "source mutation occurs during old owned estimation");
    std::fill(mutableSource.begin(), mutableSource.end(), 0.0);
    process(changed, mutableSource, reference.scale, reference.rate, silence, prefix.size());
    require(std::ranges::all_of(std::span{silence}.subspan(2 * reference.latencyFrames() + 8192),
        [](double sample) { return std::abs(sample) < 1e-12; }), "late old-source model cannot reintroduce an erased source");
    allocations = 0; countAllocations = true; changed.reset(); countAllocations = false;
    require(allocations == 0 && changed.componentModelMix() == 0 && !changed.componentAnalysisPending(),
        "reset invalidates active component and pending analysis without allocation");
    process(changed, mutableSource, reference.scale, reference.rate, silence);
    require(std::ranges::all_of(silence, [](double sample) { return sample == 0; }), "reset has no model tail");
    std::cout << "component_model checkpoints=3 source_change=1 reset=1 noalloc=1\n";
}

void testComponentModelFractionalSourceClock()
{
    std::size_t failures = 0;
    for (const auto rate : {.73, 1.371})
        for (const auto frequency : {2203.0, 7883.0, 17011.0})
        {
            const auto source = tone(frequency, .1, 9 * 48000);
            Processor processor(sampleRate, 128, 8192, .25, rate);
            std::vector<double> warmup(3 * 48000), output(48000);
            process(processor, source, .25, rate, warmup);
            double minimumMix = 1;
            for (std::size_t offset = 0; offset < output.size(); offset += 128)
            {
                process(processor, source, .25, rate,
                    std::span(output).subspan(offset, std::min<std::size_t>(128, output.size() - offset)),
                    warmup.size() + offset);
                minimumMix = std::min(minimumMix, processor.componentModelMix());
            }
            const auto amplitude = 4 * magnitude(output, frequency * .25);
            const auto mean = std::accumulate(output.begin(), output.end(), 0.0) / output.size();
            const auto valid = minimumMix == 1 && std::abs(amplitude - .25) < .001
                && std::abs(mean - .1) < .0001;
            std::cout << "component_fractional rate=" << rate << " frequency=" << frequency
                << " min_mix=" << minimumMix << " amplitude=" << amplitude
                << " mean=" << mean << " pass=" << valid << std::endl;
            if (!valid) ++failures;
        }
    require(failures == 0, "fractional source clocks preserve stationary model confidence, pitch and gain");
}

void testComponentModelMaximumFrameWork()
{
    constexpr std::array legacyFrequencies{91.1,171.3,251.9,332.3,411.7,493.1,573.7,653.9};
    for (const auto count : {8U,16U}) for (const auto window : {8192U, 65536U})
    {
        std::vector<double> frequencies(count);
        for (std::size_t k = 0; k < count; ++k)
            frequencies[k] = count == 8 ? legacyFrequencies[k] : 30+32*k;
        std::vector<double> source(40 * 48000, .1);
        for (std::size_t i = 0; i < source.size(); ++i)
            for (std::size_t partial = 0; partial < frequencies.size(); ++partial)
                source[i] += .25 / (partial + 1) * std::sin(tau * frequencies[partial]
                    * 8192 / window * i / sampleRate + .37 - .63 * partial);
        // Rate2 requires the extra reference FFT. Sixteen components exercise
        // the largest validation and reconstruction work on every frame.
        Processor processor(sampleRate, 128, window, .5, 2);
        std::array<double, 128> scratch{};
        std::size_t position = 0;
        while (processor.componentModelMix() != 1 && position < 16 * 48000)
        {
            process(processor, source, .5, 2, scratch, position);
            position += scratch.size();
        }
        require(processor.componentModelMix() == 1, "stationary source reaches full confidence within the fixed startup bound");
        std::vector<double> output(3 * window);
        process(processor, source, .5, 2, output, position);
        require(processor.componentModelMix() == 1, "maximum component work finishes before each next capture");
        const auto measured = std::span(output).last(2 * window);
        for (std::size_t partial = 0; partial < frequencies.size(); ++partial)
            require(std::abs(4 * magnitude(measured, frequencies[partial] * 8192 / window * .5)
                - .25 / (partial + 1)) < .002, "maximum-work synthesis retains every component gain");
        std::cout << "component_work window=" << window << " partials=" << count << " reference_fft=1 ready_frame="
            << position << " pass=1 noalloc=1\n";
    }
}

void testDenseComponentFrameWork()
{
    std::size_t cases = 0;
    for (const auto window : {1024U,2048U,4096U,8192U,16384U,32768U,65536U})
    for (std::size_t count = 9; count <= 16; ++count)
    {
        std::vector<double> frequencies(count), source(12*window, .1);
        for (std::size_t k = 0; k < count; ++k)
        {
            frequencies[k] = (30+32*k)*8192.0/window;
            for (std::size_t i = 0; i < source.size(); ++i)
                source[i] += .2/(k+1)*std::sin(tau*frequencies[k]*i/sampleRate + .37-.63*k);
        }
        for (const auto rate : {1.0,2.0})
        {
            Processor processor(sampleRate, 128, window, .5, rate);
            talas::dsp::PreparedTimeFrequencyScaleTestAccess::seedCandidate(processor, frequencies);
            std::vector<double> output(4*window);
            process(processor, source, .5, rate, output);
            require(processor.componentModelMix() == 1,
                "dense frame scheduling retains a current valid model after its full activation ramp");
            require(std::ranges::all_of(output, [](double value) { return std::isfinite(value); }),
                "dense scheduled output remains finite");
            if (window == 65536 && count == 16 && rate == 2)
            {
                require(talas::dsp::PreparedTimeFrequencyScaleTestAccess::startAnalysis(processor, source),
                    "largest active frame admits a simultaneous source analysis");
                std::vector<double> nextHop(window/4);
                process(processor, source, .5, rate, nextHop, output.size());
                const auto work = talas::dsp::PreparedTimeFrequencyScaleTestAccess::analysisWork(processor);
                require(work > 0 && processor.componentAnalysisPending(),
                    "largest dense reference frame leaves progress for a pending source analysis");
                std::cout << "dense_frame_model_progress window=" << window << " work=" << work << '\n';
            }
            ++cases;
        }
        std::cout << "dense_frame_work window=" << window << " partials=" << count
            << " rates=2 pass=1 noalloc=1\n";
    }
    require(cases == 112, "all seven windows and eight larger capacities cover both reference branches");
    std::cout << "dense_frame_work cases=" << cases << " total_work_per_sample=320 pass=1\n";
}

void probeLowPartialsWindowResolution()
{
    using Reference = talas::test::TimeFrequencyScaleLowPartialsReference;
    for (const std::size_t window : {16384, 32768})
    {
        std::size_t passes = 0;
        for (const auto scale : {0.0625, 0.125})
            for (const auto rate : {0.25, 0.75})
                for (const auto dc : {0.0, 0.1})
                    for (const auto quietFundamental : {false, true})
                        for (const auto shiftedPhases : {false, true})
                        {
                            const Reference reference{28, scale, rate, dc, quietFundamental, shiftedPhases, window};
                            const auto source = reference.recording();
                            Processor processor(sampleRate, 128, window, scale, rate);
                            std::vector<double> output(reference.outputFrames());
                            process(processor, source, scale, rate, output);
                            if (reference.verify(output, "window-diagnostic")) ++passes;
                        }
        // Diagnostic only: this is not called by default CTest and does not
        // replace the still-failing N8192 acceptance oracle.
        std::cout << "low_partials window-diagnostic window=" << window << " passing=" << passes
            << " total=32" << std::endl;
    }
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--dense-frame-work-probe")
        { testDenseComponentFrameWork(); return 0; }
        if (argc == 2 && std::string_view(argv[1]) == "--dense-partials-probe")
        {
            testDensePartialsRetainIndependentPitchAndGain();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--low-partials-window-probe")
        {
            probeLowPartialsWindowResolution();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--low-partials-probe")
        {
            testLowPartialsRetainIndependentPitchAndGain();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--component-state-probe")
        {
            testComponentModelContinuationAndCurrentSource();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--processing-state-probe")
        {
            testComponentModelContinuationAndCurrentSource();
            testPendingFrameRetainsItsLiveCapture();
            testResetDiscardsPendingAndWrappedState();
            testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination();
            testNonFiniteAndOverflowRejection();
            testPartitionResetCheckpointAndNeutral();
            std::cout << "TimeFrequencyScale processing-state tests passed\n";
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--component-fractional-probe")
        {
            testComponentModelFractionalSourceClock();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--component-work-probe")
        {
            testComponentModelMaximumFrameWork();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--low-output-probe")
        {
            testDownwardPitchNearDc();
            testMovedNyquistEndpointRetainsAmplitude();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--independent-controls-probe")
        {
            testIndependentFrequencyAndTimeControls();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--variable-source-probe")
        {
            testChangingRateRetainsIndependentSourceTimeAndPitch();
            return 0;
        }
        talas::test::ShardRunner run{argc, argv};
        run(testLowPartialsRetainIndependentPitchAndGain);
        run(testComponentModelContinuationAndCurrentSource);
        run(testComponentModelFractionalSourceClock);
        run(testComponentModelMaximumFrameWork);
        run(testDownwardPitchNearDc);
        run(testMovedNyquistEndpointRetainsAmplitude);
        run(testIndependentFrequencyAndTimeControls);
        run(testChangingRateRetainsIndependentSourceTimeAndPitch);
        run(testLongFftAdmissionAndRoundTrip);
        run(testLongFftMatchesStandardProfile<double>);
        run(testLongFftMatchesStandardProfile<float>);
        run(testIncrementalFftAndPendingState);
        run(testPendingFrameRetainsItsLiveCapture);
        run(testResetDiscardsPendingAndWrappedState);
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(65536, 65536); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(32768, 32768); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(16384, 16384); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(8192, 8192); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(4096, 4096); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(2048, 2048); });
        run([] { testStageAwareCheckpointsPreserveChangingClockAndDirtyDestination(1024, 1024); });
        run(testNonFiniteAndOverflowRejection);
        run(testLowTonesAndDc);
        run(testIndependentPitchAndTwoPartials);
        run(testPartitionResetCheckpointAndNeutral);
        run(testOriginalRateWindowsRespectLiveSourceAvailability);
        run(testFastSourceHopRetainsOffBinPitch);
        run.report("TimeFrequencyScale source-window");
        std::cout << "TimeFrequencyScale source-window tests passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
