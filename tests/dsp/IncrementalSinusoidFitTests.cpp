#include "talas/dsp/IncrementalSinusoidFit.h"
#include "../support/LagSubspaceStationaryReference.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace { std::atomic<bool> counting = false; std::atomic<std::size_t> allocations = 0; }
void* operator new(std::size_t size)
{
    if (counting.load(std::memory_order_relaxed)) ++allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace
{
using Fit = talas::dsp::IncrementalSinusoidFit;
using Samples = std::array<double, Fit::maximumSamples>;
constexpr auto tau = 2 * std::numbers::pi;
static_assert(std::is_trivially_copyable_v<Fit>);
static_assert(Fit::maximumColumns == 49);
static_assert(Fit::maximumWorkForSize(Fit::maximumSamples, Fit::maximumPartials) == 466'971'271);
static_assert(Fit::maximumWorkForSize(Fit::maximumSamples, Fit::maximumPartials)
              <= std::numeric_limits<std::uint32_t>::max());
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class Action> auto noAllocation(Action&& action)
{
    allocations = 0; counting = true;
    const auto result = action(); counting = false;
    require(allocations == 0, "capture/advance/copy/reset allocate nothing"); return result;
}
void finish(Fit& state, std::size_t partition = 8192)
{
    auto previous = state.consumedWork();
    while (state.status() == Fit::Status::Running)
    {
        auto work = partition;
        noAllocation([&] { state.advance(work); return true; });
        require(work <= partition && state.consumedWork() - previous == partition - work,
                "every operation consumes supplied work exactly");
        require(state.consumedWork() > previous && state.consumedWork() <= state.maximumWork(), "bounded progress");
        previous = state.consumedWork();
    }
}
Samples waveform(std::size_t count, double rate, std::span<const double> frequencies,
                 std::span<const double> amplitudes, double dc = .1, double center = 3.141)
{
    Samples samples{};
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto time = center + (static_cast<double>(i) - count / 2.0) / rate;
        samples[i] = dc;
        for (std::size_t j = 0; j < frequencies.size(); ++j)
            samples[i] += amplitudes[j] * std::sin(tau * frequencies[j] * time + .73 - 1.83 * j);
    }
    return samples;
}
void same(const Fit& first, const Fit& second, bool validation = true)
{
    require(first.status() == second.status() && first.size() == second.size()
            && first.refinements() == second.refinements() && first.dc() == second.dc()
            && first.trainingRelativeRms() == second.trainingRelativeRms()
            && first.componentRmsRatio() == second.componentRmsRatio()
            && first.conditionBound() == second.conditionBound(), "exact training state");
    if (validation) require(first.accepted() == second.accepted() && first.validationRelativeRms() == second.validationRelativeRms()
                           && first.consumedWork() == second.consumedWork(), "exact completed state");
    for (std::size_t i = 0; i < first.size(); ++i)
        require(first.frequency(i) == second.frequency(i) && first.cosineCoefficient(i) == second.cosineCoefficient(i)
                && first.sineCoefficient(i) == second.sineCoefficient(i), "exact fitted parameters");
}
void arrayJson(std::span<const double> values)
{
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) std::cout << (i ? "," : "") << values[i];
    std::cout << ']';
}
void dump(const Fit& state, std::span<const double> samples, double rate,
          std::span<const double> candidates, std::string_view name)
{
    std::array<double, Fit::maximumPartials> frequencies{};
    std::array<double, 1 + 2 * Fit::maximumPartials> coefficients{};
    coefficients[0] = state.dc();
    for (std::size_t i = 0; i < state.size(); ++i)
    { frequencies[i] = state.frequency(i); coefficients[1 + i] = state.cosineCoefficient(i); coefficients[1 + state.size() + i] = state.sineCoefficient(i); }
    std::cout << std::setprecision(17) << "{\"name\":\"" << name << "\",\"rate\":" << rate << ",\"samples\":";
    arrayJson(samples); std::cout << ",\"initial\":"; arrayJson(candidates);
    std::cout << ",\"frequencies\":"; arrayJson(std::span{frequencies}.first(state.size()));
    std::cout << ",\"coefficients\":"; arrayJson(std::span{coefficients}.first(1 + 2 * state.size()));
    std::cout << ",\"training_rms\":" << state.trainingRelativeRms() << ",\"validation_rms\":" << state.validationRelativeRms()
              << ",\"component_ratio\":" << state.componentRmsRatio() << ",\"condition_bound\":" << state.conditionBound()
              << ",\"refinements\":" << state.refinements() << "}" << std::endl;
}

void testPositiveFixtures(bool output)
{
    std::size_t cases = 0, maxWork = 0, refined = 0;
    double maxFrequency = 0, maxAmplitude = 0, maxDc = 0, maxMs = 0, maxCondition = 0;
    auto run = [&](std::span<const double> samples, double rate, std::span<const double> truth,
                   std::span<const double> amplitudes, double dc, bool perturb, double units, std::string_view name)
    {
        std::array<double, Fit::maximumPartials> candidates{};
        for (std::size_t i = 0; i < truth.size(); ++i) candidates[i] = truth[i] + (perturb ? (i % 2 == 0 ? .025 : -.025) : 0);
        const auto candidateSpan = std::span{candidates}.first(truth.size());
        Fit state;
        require(noAllocation([&] { return state.begin(samples, rate, candidateSpan); }), "valid fit capture");
        const auto start = std::chrono::steady_clock::now(); finish(state);
        maxMs = std::max(maxMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        if (!state.accepted()) std::cerr << "rejected " << name << " case=" << cases << " status=" << static_cast<int>(state.status())
            << " train=" << state.trainingRelativeRms() << " validation=" << state.validationRelativeRms()
            << " ratio=" << state.componentRmsRatio() << " refinements=" << state.refinements() << std::endl;
        require(state.status() == Fit::Status::Ready && state.accepted(), "every declared stationary model accepted");
        for (std::size_t i = 0; i < truth.size(); ++i)
        {
            const auto frequencyError = std::abs(state.frequency(i) - truth[i]);
            const auto amplitude = std::hypot(state.cosineCoefficient(i), state.sineCoefficient(i));
            const auto amplitudeError = std::abs(amplitude / units / amplitudes[i] - 1);
            if (frequencyError > .02 || amplitudeError > .002) std::cerr << name << " frequency=" << frequencyError << " amplitude=" << amplitudeError << std::endl;
            require(frequencyError <= .02 && amplitudeError <= .002, "independent frequency/amplitude gate");
            maxFrequency = std::max(maxFrequency, frequencyError); maxAmplitude = std::max(maxAmplitude, amplitudeError);
        }
        const auto dcError = std::abs(state.dc() / units - dc);
        require(dcError <= 1e-6, "independent DC gate"); maxDc = std::max(maxDc, dcError);
        require(state.refinements() <= Fit::maximumRefinements, "refinement cap");
        refined += state.refinements() != 0; maxWork = std::max(maxWork, state.consumedWork());
        maxCondition = std::max(maxCondition, state.conditionBound()); ++cases;
        if (output) dump(state, samples, rate, candidateSpan, name);
    };
    talas::test::lag_subspace::forEachProfile([&](auto frequencies, auto amplitudes, bool quantized, double center, auto name)
    {
        auto samples = waveform(8192, 48000, frequencies, amplitudes, .1, center);
        if (quantized) for (std::size_t i = 0; i < 8192; ++i) samples[i] = std::round(samples[i] * 32768) / 32768;
        run(std::span{samples}.first(8192), 48000, frequencies, amplitudes, .1, true, 1, name);
    });
    for (const auto base : {20.0, 173.2, 13001.0})
        for (const auto swapped : {false, true})
            for (const auto precision : {0, 1, 2})
            {
                const std::array frequencies{base, base + 8};
                const auto amplitudes = swapped ? std::array{.08, .25} : std::array{.25, .08};
                auto samples = waveform(8192, 48000, frequencies, amplitudes);
                std::uint32_t seed = 271828;
                for (std::size_t i = 0; i < 8192; ++i)
                {
                    seed = 1664525U * seed + 1013904223U;
                    if (precision == 0) samples[i] = static_cast<float>(samples[i]);
                    else samples[i] += (precision == 1 ? 1e-6 : 1e-5) * std::sqrt(12.0)
                        * (static_cast<double>(seed) / 4294967296.0 - .5);
                }
                run(std::span{samples}.first(8192), 48000, frequencies, amplitudes, .1, true, 1, "sensitivity");
            }
    const std::array frequencies{173.2, 181.6}, amplitudes{.25, .08};
    for (const auto count : {1024U, 8192U, 65536U})
        for (const auto rate : {44100.0, 48000.0, 96000.0})
        {
            const auto samples = waveform(count, rate, frequencies, amplitudes);
            run(std::span{samples}.first(count), rate, frequencies, amplitudes, .1, false, 1, "size_rate");
        }
    for (const auto scale : {1e-200, 1e200})
    {
        auto samples = waveform(8192, 48000, frequencies, amplitudes);
        for (auto& sample : samples) sample *= scale;
        run(std::span{samples}.first(8192), 48000, frequencies, amplitudes, .1, false, scale, "uniform_scale");
    }
    for (const auto dc : {0.0, .25, -.7})
    {
        Samples samples{}; samples.fill(dc);
        run(std::span{samples}.first(8192), 48000, {}, {}, dc, false, 1, "constant");
    }
    require(cases == 68 && refined >= 54, "all declared stationary and refinement fixtures");
    if (!output) std::cout << "positive_cases=" << cases << " refined_cases=" << refined << " max_frequency_hz=" << maxFrequency
        << " max_relative_amplitude=" << maxAmplitude << " max_dc=" << maxDc << " max_condition_bound=" << maxCondition
        << " max_work=" << maxWork << " max_whole_job_ms=" << maxMs << std::endl;
}

void testRejections()
{
    std::size_t rejected = 0;
    const std::array one{173.0}, amplitude{.25};
    for (const auto kind : {0,1,2,3,4,5,6,7,8})
    {
        auto samples = waveform(8192, 48000, one, amplitude, .1, 8192.0 / 2 / 48000);
        std::uint32_t seed = 314159;
        for (std::size_t i = 0; i < 8192; ++i)
        {
            const auto time = static_cast<double>(i) / 48000;
            seed = 1664525U * seed + 1013904223U;
            const auto noise = static_cast<double>(seed) / 4294967296.0 - .5;
            if (kind == 0) samples[i] = .1 + .25 * std::sin(tau * (173 * time + 40 * time * time) + .73);
            if (kind == 1 || kind == 8) samples[i] = .1 + .25 * (1 + 2 * time) * std::sin(tau * 173 * time + .73);
            if (kind == 2 && i >= 3000) samples[i] = .1 + .5 * (samples[i] - .1);
            if (kind == 3) samples[i] = i == 3000 ? 1 : 0;
            if (kind == 4) samples[i] = noise;
            if (kind == 5 && i >= 6144) samples[i] += .1;
            if (kind == 6) samples[i] += .01 * noise;
        }
        const std::array wrong{1673.0};
        const std::array cancellation{172.999995, 173.000005};
        const auto candidates = kind == 8 ? std::span<const double>{cancellation}
            : kind == 7 ? std::span<const double>{wrong} : std::span<const double>{one};
        Fit state;
        require(state.begin(std::span{samples}.first(8192), 48000, candidates), "negative fixture capture"); finish(state);
        require(!state.accepted(), "nonstationary/wrong/cancelling models rejected");
        if (kind == 8)
            require(state.status() == Fit::Status::Ready && state.componentRmsRatio() > 4 && state.refinements() == 0,
                    "cancellation gate prevents unstable frequency refinement");
        ++rejected;
    }
    Samples constant{};
    Fit state;
    constant.fill(.25); constant[8191] = .250001;
    require(state.begin(std::span{constant}.first(8192), 48000, {}), "changed constant capture"); finish(state);
    require(!state.accepted(), "constant validation must not train DC");
    std::cout << "negative_cases=" << rejected << " changed_constant_rejected=1" << std::endl;
}

void testStateAndAdmission()
{
    const std::array truth{20.0, 28.0}, amplitudes{.25, .08};
    auto samples = waveform(8192, 48000, truth, amplitudes);
    std::array candidates{20.025, 27.975};
    Fit reference;
    auto work = std::size_t{31}; reference.advance(work);
    require(work == 31 && reference.maximumWork() == 0 && !reference.accepted(), "idle fit inert");
    require(reference.begin(std::span{samples}.first(8192), 48000, candidates), "reference capture"); finish(reference);
    require(reference.accepted(), "checkpoint reference accepted");
    for (const auto partition : {1U, 97U, 65536U})
    {
        Fit split;
        require(split.begin(std::span{samples}.first(8192), 48000, candidates), "partition capture");
        auto zero = std::size_t{0}; split.advance(zero);
        require(split.consumedWork() == 0 && split.dc() == 0 && split.frequency(0) == 0 && !split.accepted(), "pending output hidden");
        finish(split, partition); same(reference, split);
    }
    std::size_t copies = 0;
    for (const auto position : {1U, 6143U, 6144U, 8191U, 8192U, 8193U, 10000U, 16383U, 16384U,
                               50000U, 200000U, 400000U, 600000U, 900000U})
    {
        Fit source, destination = reference;
        require(source.begin(std::span{samples}.first(8192), 48000, candidates), "pending copy capture");
        auto budget = static_cast<std::size_t>(position); source.advance(budget);
        if (source.status() != Fit::Status::Running) continue;
        noAllocation([&] { destination = source; source.reset(); return true; }); finish(destination, 701);
        same(reference, destination); ++copies;
    }
    require(copies >= 8, "several statistics/QR/refinement checkpoints");
    Fit owned;
    require(owned.begin(std::span{samples}.first(8192), 48000, candidates), "owned input capture");
    samples.fill(std::numeric_limits<double>::quiet_NaN()); candidates.fill(std::numeric_limits<double>::quiet_NaN());
    finish(owned); same(reference, owned);
    samples = waveform(8192, 48000, truth, amplitudes); candidates = {20.025, 27.975};
    for (std::size_t i = 6144; i < 8192; ++i) samples[i] = 1e6;
    require(owned.begin(std::span{samples}.first(8192), 48000, candidates), "validation-only peak change"); finish(owned);
    same(reference, owned, false); require(!owned.accepted(), "validation change affects acceptance only");
    // Alias ranking must never inspect the heldout quarter, even for admission
    // or normalization. A training-only result is never a validated model.
    Fit training;
    require(training.begin(std::span{samples}.first(8192), 48000, candidates, true, false), "training-only capture"); finish(training);
    require(training.status() == Fit::Status::Ready && !training.accepted() && !training.validationPerformed(),
            "training-only result cannot claim validation");
    same(reference, training, false);
    for (const auto value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max()})
    {
        for (std::size_t i = 6144; i < 8192; ++i) samples[i] = value;
        require(owned.begin(std::span{samples}.first(8192), 48000, candidates, true, false), "invisible heldout capture"); finish(owned);
        same(training, owned);
        require(!owned.validationPerformed(), "heldout was not inspected");
    }
    require(reference.validationPerformed(), "default API retains validation");
    require(!owned.begin({}, 48000, {}) && !owned.begin(std::span{samples}.first(31), 48000, {})
            && !owned.begin(std::span{samples}.first(33), 48000, {})
            && !owned.begin(std::span{samples}.first(8192), 0, {}), "invalid dimensions/rate");
    const std::array<double, Fit::maximumSamples + 1> oversized{};
    std::array<double, Fit::maximumPartials + 1> tooMany{};
    for (std::size_t i = 0; i < tooMany.size(); ++i) tooMany[i] = 100.0 + i;
    require(!owned.begin(oversized, 48000, {})
            && !owned.begin(std::span{samples}.first(8192), 48000, tooMany), "capture capacities are closed");
    for (const auto rate : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        require(!owned.begin(std::span{samples}.first(8192), rate, {}), "nonfinite/negative sample rates rejected");
    for (const auto invalid : {std::array{0.0, 28.0}, std::array{20.0, 24000.0}, std::array{28.0, 20.0}, std::array{20.0, 20.0},
                              std::array{20.0, std::numeric_limits<double>::infinity()}})
        require(!owned.begin(std::span{samples}.first(8192), 48000, invalid), "invalid candidate frequencies");
    samples[0] = std::numeric_limits<double>::quiet_NaN();
    require(owned.begin(std::span{samples}.first(8192), 48000, candidates), "raw capture defers sample validation"); finish(owned);
    require(owned.status() == Fit::Status::InvalidInput && !owned.accepted() && owned.dc() == 0, "nonfinite sample rejected before fitting");
    samples.fill(1); samples[1] = 1e-300; samples[0] = 1e300;
    require(owned.begin(std::span{samples}.first(8192), 48000, candidates), "dynamic-range raw capture"); finish(owned);
    require(owned.status() == Fit::Status::DynamicRangeLoss && !owned.accepted(), "normalization loss rejected");
    samples = waveform(8192, 48000, truth, amplitudes);
    const std::array singular{.000001, .000003};
    require(owned.begin(std::span{samples}.first(8192), 48000, singular), "near-DC ill-conditioned design capture"); finish(owned);
    require(owned.status() == Fit::Status::IllConditioned && !owned.accepted() && owned.dc() == 0, "ill-conditioned design fails closed");
    const std::array tinyFrequencies{3000.0, 7000.0};
    samples = waveform(32, 48000, tinyFrequencies, amplitudes);
    require(owned.begin(std::span{samples}.first(32), 48000, tinyFrequencies), "minimum window admission"); finish(owned);
    require(owned.accepted(), "minimum window exact fit");
    const std::array overflowFrequency{1500.0};
    for (std::size_t i = 0; i < 8192; ++i)
        samples[i] = std::copysign(std::numeric_limits<double>::max(), std::cos(tau * 1500 * i / 48000));
    require(owned.begin(std::span{samples}.first(8192), 48000, overflowFrequency), "finite data with unrepresentable fitted coefficient"); finish(owned);
    require(owned.status() == Fit::Status::NumericOverflow && !owned.accepted() && owned.cosineCoefficient(0) == 0,
            "output coefficient overflow fails closed");
    const std::array manyFrequencies{1200., 3800., 6400., 9000., 11600., 14200., 16800., 19400.};
    std::array<double, 8> manyAmplitudes{}, manyCandidates{};
    for (std::size_t i = 0; i < 8; ++i) { manyAmplitudes[i] = .25 / (i + 1); manyCandidates[i] = manyFrequencies[i] + 20; }
    samples = waveform(32, 48000, manyFrequencies, manyAmplitudes);
    Fit unrefined;
    require(unrefined.begin(std::span{samples}.first(32), 48000, manyCandidates, false), "baseline linear fit"); finish(unrefined);
    require(owned.begin(std::span{samples}.first(32), 48000, manyCandidates, true), "rank-deficient refinement Jacobian"); finish(owned);
    //24 training observations cannot identify25 Jacobian columns, while the
    //17-column linear model is full rank. Refinement must preserve that fit.
    require(unrefined.status() == Fit::Status::Ready && owned.status() == Fit::Status::Ready
            && owned.refinements() == 0 && owned.consumedWork() > unrefined.consumedWork(), "failed refinement attempted and stopped");
    same(unrefined, owned, false);
    require(unrefined.validationRelativeRms() == owned.validationRelativeRms()
            && unrefined.accepted() == owned.accepted(), "failed refinement retains the prior validation/model decision");
    noAllocation([&] { owned.reset(); return true; });
    require(owned.status() == Fit::Status::Idle && owned.maximumWork() == 0 && owned.frequency(0) == 0, "scalar reset hides results");
    std::cout << "checkpoint_positions=" << copies << " sizeof_fit=" << sizeof(Fit) << std::endl;
}

void testDenseCapacity()
{
    const std::array frequencies{
        83.17, 151.43, 228.91, 307.27, 388.61, 472.19, 559.73, 649.31,
        743.87, 842.53, 947.11, 1058.29, 1175.83, 1299.47, 1434.23, 1579.61};
    const std::array amplitudes{
        .25, .14, .09, .065, .05, .04, .033, .028,
        .024, .021, .019, .017, .015, .013, .011, .009};
    Samples samples{};
    std::size_t refinedCases = 0;
    for (const auto count : {9U, 12U, 16U})
    {
        const auto truth = std::span{frequencies}.first(count);
        const auto sourceAmplitudes = std::span{amplitudes}.first(count);
        samples = waveform(8192, 48000, truth, sourceAmplitudes);
        std::array<double, Fit::maximumPartials> candidates{};
        for (std::size_t i = 0; i < count; ++i)
            candidates[i] = truth[i] + (i % 2 == 0 ? .025 : -.025);

        Fit state;
        require(noAllocation([&] { return state.begin(std::span{samples}.first(8192), 48000,
                                                        std::span{candidates}.first(count)); }),
                "dense exact-source capture");
        if (count == Fit::maximumPartials)
        {
            auto firstBudget = std::size_t{131072};
            noAllocation([&] { state.advance(firstBudget); return true; });
            require(firstBudget == 0 && state.status() == Fit::Status::Running
                    && state.consumedWork() == 131072, "dense fit continues from a bounded work slice");

            Fit continuation;
            const auto checkpointCopied = noAllocation([&]
            {
                const auto captured = continuation.begin(std::span{samples}.first(8192), 48000,
                                                           std::span{candidates}.first(count));
                auto dirtyBudget = std::size_t{131072};
                if (captured) continuation.advance(dirtyBudget);
                const auto storageDirtied = captured && dirtyBudget == 0
                    && continuation.status() == Fit::Status::Running
                    && continuation.consumedWork() == 131072;
                continuation.reset();
                continuation = state;
                return storageDirtied && continuation.status() == Fit::Status::Running
                    && continuation.consumedWork() == state.consumedWork();
            });
            require(checkpointCopied, "dense running checkpoint overwrites dirtied fixed storage without allocation");
            finish(state, 32768);
            finish(continuation, 49152);
            same(state, continuation);
        }
        else finish(state, 32768);
        require(state.status() == Fit::Status::Ready && state.accepted(), "dense exact-source fit accepted");
        require(state.refinements() != 0, "off-bin dense candidates refined");
        ++refinedCases;
        for (std::size_t i = 0; i < count; ++i)
        {
            require(std::abs(state.frequency(i) - truth[i]) <= .005, "dense independent frequency gate");
            const auto fittedAmplitude = std::hypot(state.cosineCoefficient(i), state.sineCoefficient(i));
            require(std::abs(fittedAmplitude / sourceAmplitudes[i] - 1) <= .002,
                    "dense independent unequal-amplitude gate");
        }
        require(std::abs(state.dc() - .1) <= 1e-6, "dense independent DC gate");
    }

    // Keep the training prefix exact and change only the held-out quarter.
    // The dense model must fit training data while rejecting validation data.
    samples = waveform(8192, 48000, frequencies, amplitudes);
    for (std::size_t i = 6144; i < 8192; ++i) samples[i] += .1;
    std::array<double, Fit::maximumPartials> candidates{};
    for (std::size_t i = 0; i < Fit::maximumPartials; ++i)
        candidates[i] = frequencies[i] + (i % 2 == 0 ? .025 : -.025);
    Fit changedValidation;
    require(noAllocation([&] { return changedValidation.begin(std::span{samples}.first(8192), 48000,
                                                               candidates); }),
            "dense changed-validation capture");
    finish(changedValidation, 32768);
    require(changedValidation.status() == Fit::Status::Ready && !changedValidation.accepted()
            && changedValidation.trainingRelativeRms() <= 1e-7
            && changedValidation.validationRelativeRms() > Fit::residualLimit,
            "dense fit rejects changed validation quarter");
    std::cout << "dense_exact_source_cases=3 refined_cases=" << refinedCases
              << " validation_rejected=1 max_work="
              << Fit::maximumWorkForSize(Fit::maximumSamples, Fit::maximumPartials) << std::endl;
}

void probeBudget()
{
    for (const auto fixture : {0, 1, 2})
    {
        const auto count = fixture == 2 ? std::size_t{65536} : std::size_t{8192};
        const auto partials = fixture == 1 ? std::size_t{8} : std::size_t{2};
        const std::array frequencies{20.0, 28.0, 171.3, 251.9, 332.3, 411.7, 493.1, 573.7};
        std::array<double, 8> amplitudes{}, candidates{};
        for (std::size_t i = 0; i < partials; ++i)
        { amplitudes[i] = .25 / (i + 1); candidates[i] = frequencies[i] + (fixture == 2 ? 0 : (i % 2 == 0 ? .025 : -.025)); }
        const auto samples = waveform(count, 48000, std::span{frequencies}.first(partials), std::span{amplitudes}.first(partials));
        Fit prepared;
        const auto captureStart = std::chrono::steady_clock::now();
        require(prepared.begin(std::span{samples}.first(count), 48000, std::span{candidates}.first(partials)), "timing capture");
        const auto captureUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - captureStart).count();
        for (const auto partition : {4096U, 16384U})
        {
            // Prepared storage and copying the checkpoint are outside the timed calls.
            Fit state = prepared;
            std::array<double, 40000> durations{};
            std::size_t calls = 0;
            while (state.status() == Fit::Status::Running)
            {
                require(calls < durations.size(), "timing storage covers bound");
                auto work = static_cast<std::size_t>(partition);
                const auto start = std::chrono::steady_clock::now(); state.advance(work);
                durations[calls++] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
                require(state.consumedWork() <= state.maximumWork(), "timed work bounded");
            }
            require(state.accepted(), "timing model accepted");
            std::sort(durations.begin(), durations.begin() + static_cast<std::ptrdiff_t>(calls));
            std::cout << "primitive_budget samples=" << count << " partials=" << partials << " budget=" << partition
                << " capture_us=" << captureUs << " calls=" << calls << " work=" << state.consumedWork()
                << " p99_us=" << durations[static_cast<std::size_t>((calls - 1) * .99)] << " max_us=" << durations[calls - 1] << std::endl;
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--dense-capacity")
        { testDenseCapacity(); return 0; }
        if (argc == 2 && std::string_view{argv[1]} == "--budget-probe") { probeBudget(); return 0; }
        const auto output = argc == 2 && std::string_view{argv[1]} == "--dump";
        testPositiveFixtures(output);
        if (!output) { testRejections(); testStateAndAdmission(); std::cout << "Incremental sinusoid fit tests passed" << std::endl; }
        return 0;
    }
    catch (const std::exception& error)
    { counting = false; std::cerr << "Incremental sinusoid fit tests failed: " << error.what() << std::endl; return 1; }
}
