#include "talas/dsp/IncrementalStationaryModel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

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

namespace talas::dsp
{
struct IncrementalStationaryModelTestAccess
{
    static std::size_t stage(const IncrementalStationaryModel& value)
    { return static_cast<std::size_t>(value.stage_); }
    static int finalOrder(const IncrementalStationaryModel& value)
    { return value.stage_ != IncrementalStationaryModel::Stage::FinalFit ? -1
        : value.constant_ ? 0 : static_cast<int>(value.order_); }
    static bool exhaustedOrder(const IncrementalStationaryModel& value)
    { return value.stage_ == IncrementalStationaryModel::Stage::Order
        && (value.order_ > 2 * IncrementalStationaryModel::maximumPartials || value.order_ > value.subspace_.rank()); }
    static std::size_t lastOrder(const IncrementalStationaryModel& value)
    { return std::min(2 * IncrementalStationaryModel::maximumPartials, value.subspace_.rank()) / 2 * 2; }
    static bool terminalFinalFit(const IncrementalStationaryModel& value)
    { return finalOrder(value) > 0 && value.order_ + 2 > lastOrder(value); }
    static bool penultimateFinalFit(const IncrementalStationaryModel& value)
    { return finalOrder(value) > 0 && value.order_ + 2 == lastOrder(value); }
    static std::size_t bucket(const IncrementalStationaryModel& value)
    {
        using Stage = IncrementalStationaryModel::Stage;
        const auto section = value.stage_ == Stage::Transform ? value.lane_
            : value.stage_ == Stage::Spectrum ? value.index_ / value.quarter_
            : value.stage_ == Stage::FinalFit ? (value.constant_ ? 3 : terminalFinalFit(value) ? 2 : penultimateFinalFit(value) ? 1 : 0)
            : exhaustedOrder(value) ? 1 : 0;
        return 4 * stage(value) + section;
    }
};
}

namespace
{
using Model = talas::dsp::IncrementalStationaryModel;
using Fft = talas::dsp::Radix2FftPlan;
constexpr auto tau = 2 * std::numbers::pi;
static_assert(std::is_trivially_copyable_v<Model>);
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <class Action> auto noAllocation(Action&& action)
{
    allocations = 0; counting = true;
    const auto result = action(); counting = false;
    require(allocations == 0, "capture/advance/copy/reset allocate nothing"); return result;
}
void finish(Model& state, const Fft& fft, std::size_t partition = 32768)
{
    auto previous = state.consumedWork();
    noAllocation([&]
    {
        while (state.status() == Model::Status::Running)
        {
            auto work = partition; state.advance(fft, work);
            require(work <= partition && state.consumedWork() - previous == partition - work, "exact work consumption");
            require(state.consumedWork() > previous && state.consumedWork() <= state.maximumWork(), "bounded progress");
            previous = state.consumedWork();
        }
        return true;
    });
}
std::vector<double> waveform(std::size_t count, double rate, std::span<const double> frequencies,
    std::span<const double> amplitudes, std::span<const double> phases, double dc, double center)
{
    std::vector<double> samples(count, dc);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto time = center + (static_cast<double>(i) - count / 2.0) / rate;
        for (std::size_t j = 0; j < frequencies.size(); ++j)
            samples[i] += amplitudes[j] * std::sin(tau * frequencies[j] * time + phases[j]);
    }
    return samples;
}
struct Noise
{
    std::uint32_t state = 314159;
    double uniform() { state = 1664525U * state + 1013904223U; return (state + .5) / 4294967296.0; }
    double gaussian() { const auto u = uniform(); return std::sqrt(-2 * std::log(u)) * std::cos(tau * uniform()); }
};
void arrayJson(std::span<const double> values)
{
    std::cout << '[';
    for (std::size_t i = 0; i < values.size(); ++i) std::cout << (i ? "," : "") << values[i];
    std::cout << ']';
}
void dump(const Model& state, std::size_t index, std::string_view name, std::span<const double> samples,
    double rate, std::span<const double> truth, std::span<const double> amplitudes, std::span<const double> phases,
    double dc, double center, double units, bool expected, bool phaseGate)
{
    std::array<double, Model::maximumPartials> frequencies{};
    std::array<double, 1 + 2 * Model::maximumPartials> coefficients{};
    std::array<double, Model::maximumGuides> guides{}, magnitudes{};
    coefficients[0] = state.dc();
    for (std::size_t i = 0; i < state.size(); ++i)
    { frequencies[i] = state.frequency(i); coefficients[1+i] = state.cosineCoefficient(i); coefficients[1+state.size()+i] = state.sineCoefficient(i); }
    for (std::size_t i = 0; i < state.guideCount(); ++i)
    { guides[i] = state.guideFrequency(i); magnitudes[i] = state.guideMagnitude(i); }
    std::cout << std::setprecision(17) << "{\"case\":" << index << ",\"name\":\"" << name << "\",\"rate\":" << rate
        << ",\"expected\":" << (expected ? "true" : "false") << ",\"accepted\":" << (state.status() == Model::Status::Ready ? "true" : "false")
        << ",\"phase_gate\":" << (phaseGate ? "true" : "false") << ",\"dc_truth\":" << dc << ",\"center\":" << center
        << ",\"units\":" << units << ",\"samples\":";
    arrayJson(samples); std::cout << ",\"truth\":"; arrayJson(truth);
    std::cout << ",\"amplitudes\":"; arrayJson(amplitudes); std::cout << ",\"phases\":"; arrayJson(phases);
    std::cout << ",\"frequencies\":"; arrayJson(std::span{frequencies}.first(state.size()));
    std::cout << ",\"coefficients\":"; arrayJson(std::span{coefficients}.first(1 + 2 * state.size()));
    std::cout << ",\"guides\":"; arrayJson(std::span{guides}.first(state.guideCount()));
    std::cout << ",\"guide_magnitudes\":"; arrayJson(std::span{magnitudes}.first(state.guideCount()));
    std::cout << ",\"training_rms\":" << state.trainingRelativeRms() << ",\"validation_rms\":" << state.validationRelativeRms()
        << ",\"orders\":" << state.attemptedOrders() << ",\"assignments\":" << state.attemptedAssignments()
        << ",\"skipped_alias_orders\":" << state.skippedAliasOrders()
        << ",\"work\":" << state.consumedWork() << "}" << std::endl;
}
struct Checks
{
    bool output = false;
    std::size_t cases = 0, accepted = 0, rejected = 0, failures = 0, maxOrders = 0, maxAssignments = 0;
    std::uint64_t maxWork = 0;
    double maxFrequency = 0, maxAmplitude = 0, maxDc = 0, maxPhase = 0, maxMs = 0;
    std::unique_ptr<Model> state = std::make_unique<Model>();
    void run(std::span<const double> samples, const Fft& fft, double rate, std::span<const double> truth,
        std::span<const double> amplitudes, std::span<const double> phases, double dc, double center,
        std::string_view name, bool expected = true, bool phaseGate = true, double units = 1)
    {
        require(noAllocation([&] { return state->begin(samples, rate); }), "valid source-only capture");
        const auto start = std::chrono::steady_clock::now(); finish(*state, fft);
        maxMs = std::max(maxMs, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        const auto ready = state->status() == Model::Status::Ready;
        bool pass = ready == expected && (ready || state->status() == Model::Status::Rejected);
        if (ready)
        {
            ++accepted; pass &= state->size() == truth.size();
            for (std::size_t i = 0; i < truth.size() && i < state->size(); ++i)
            {
                const auto frequencyError = std::abs(state->frequency(i) - truth[i]);
                const auto amplitudeError = std::abs(std::hypot(state->cosineCoefficient(i), state->sineCoefficient(i)) / units / amplitudes[i] - 1);
                const auto centerPhase = std::atan2(state->cosineCoefficient(i), state->sineCoefficient(i))
                    + tau * state->frequency(i) * samples.size() / 2 / rate;
                const auto phaseError = std::abs(std::remainder(centerPhase - (tau * truth[i] * center + phases[i]), tau));
                pass &= frequencyError <= .02 && amplitudeError <= .002 && (!phaseGate || phaseError <= 1e-4);
                maxFrequency = std::max(maxFrequency, frequencyError); maxAmplitude = std::max(maxAmplitude, amplitudeError);
                if (phaseGate) maxPhase = std::max(maxPhase, phaseError);
            }
            const auto dcError = std::abs(state->dc() / units - dc); maxDc = std::max(maxDc, dcError); pass &= dcError <= 1e-6;
        }
        else
        {
            ++rejected;
            require(state->size() == 0 && state->dc() == 0 && state->frequency(0) == 0 && state->cosineCoefficient(0) == 0
                && state->sineCoefficient(0) == 0 && state->trainingRelativeRms() == 0 && state->validationRelativeRms() == 0, "failed model hidden");
        }
        if (!pass)
        {
            ++failures;
            std::cerr << "profile_failed case=" << cases << " name=" << name << " status=" << static_cast<int>(state->status())
                << " size=" << state->size() << " expected_size=" << truth.size() << " expected_accept=" << expected
                << " train=" << state->trainingRelativeRms() << " validation=" << state->validationRelativeRms()
                << " orders=" << state->attemptedOrders() << " assignments=" << state->attemptedAssignments() << std::endl;
        }
        maxWork = std::max(maxWork, state->consumedWork()); maxOrders = std::max(maxOrders, state->attemptedOrders());
        maxAssignments = std::max(maxAssignments, state->attemptedAssignments());
        if (output) dump(*state, cases, name, samples, rate, truth, amplitudes, phases, dc, center, units, expected, phaseGate);
        ++cases;
    }
    void report() const
    {
        if (!output) std::cout << "source_profiles=" << cases << " accepted=" << accepted << " rejected=" << rejected << " failures=" << failures
            << " max_frequency_hz=" << maxFrequency << " max_relative_amplitude=" << maxAmplitude << " max_dc=" << maxDc
            << " max_clean_phase_rad=" << maxPhase << " max_orders=" << maxOrders << " max_assignments=" << maxAssignments
            << " max_work=" << maxWork << " max_whole_job_ms=" << maxMs << std::endl;
    }
};

void testProfiles(bool output, bool stress)
{
    const Fft fft(2048, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
    Checks checks; checks.output = output;
    for (const auto upper : {40.0, 28.0}) for (const auto rate : {.25, .75}) for (const auto dc : {0.0, .1})
    for (const auto swapped : {false, true}) for (const auto shifted : {false, true})
    {
        const std::array frequencies{20.0, upper}, amplitudes = swapped ? std::array{.08, .25} : std::array{.25, .08};
        const auto phases = shifted ? std::array{.73, -1.1} : std::array{0.0, 0.0};
        if (stress)
        {
            for (std::size_t k = 0; k < 260; ++k)
            {
                const auto logicalCenter = 4096 + k * 2048;
                if (logicalCenter < 3 * 48000 || logicalCenter > 11 * 48000) continue;
                const auto center = rate * logicalCenter / 48000;
                const auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, dc, center);
                checks.run(samples, fft, 48000, frequencies, amplitudes, phases, dc, center, "contiguous");
            }
            std::cerr << "contiguous_progress cases=" << checks.cases << " failures=" << checks.failures << std::endl;
        }
        else for (const auto seconds : {3.0, 5.0, 9.0})
        {
            const auto logicalCenter = 4096 + std::round((seconds * 48000 - 4096) / 2048) * 2048;
            const auto center = rate * logicalCenter / 48000;
            const auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, dc, center);
            checks.run(samples, fft, 48000, frequencies, amplitudes, phases, dc, center, "selected");
        }
    }
    if (stress)
    { checks.report(); require(checks.cases == 5984 && checks.failures == 0, "contiguous source-window gate"); return; }
    const std::array<std::vector<double>, 18> bands{{{17.3,25.7},{173.2,181.1},{999.3,1007.7},{4999.3,5007.7},
        {13001,13009},{200,414.9,901.3},{27.1,41.3,76.4},{400,409,430},
        {91.1},{91.1,171.3},{91.1,171.3,251.9},{91.1,171.3,251.9,332.3},
        {91.1,171.3,251.9,332.3,411.7},{91.1,171.3,251.9,332.3,411.7,493.1},
        {91.1,171.3,251.9,332.3,411.7,493.1,573.7},{91.1,171.3,251.9,332.3,411.7,493.1,573.7,653.9},
        {64,1564},{89,1589}}};
    for (std::size_t band = 0; band < bands.size(); ++band) for (const auto center : {.731, 3.141})
    {
        std::array<double, 8> amplitudes{}, phases{};
        for (std::size_t i = 0; i < bands[band].size(); ++i) { amplitudes[i] = .25 / (1+i); phases[i] = .37 - .63 * i; }
        const auto amps = std::span{amplitudes}.first(bands[band].size()), phs = std::span{phases}.first(bands[band].size());
        const auto samples = waveform(8192, 48000, bands[band], amps, phs, .1, center);
        checks.run(samples, fft, 48000, bands[band], amps, phs, .1, center, "broader", band < 16);
    }
    Noise noise;
    for (const auto frequencies : {std::array{20.0,28.0}, std::array{173.2,181.1}, std::array{4999.3,5007.7}})
    for (const auto ratio : {.32, .1, .01}) for (const auto center : {.731, 3.141}) for (const auto precision : {0,1,2,3,4,5})
    {
        const std::array amplitudes{.25, .25 * ratio}, phases{.73, -1.1};
        auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, center);
        for (auto& value : samples)
        {
            if (precision == 1) value = static_cast<float>(value);
            if (precision == 2 || precision == 3) { const auto quantum = precision == 2 ? 8388608.0 : 32768.0; value = std::round(value * quantum) / quantum; }
            if (precision >= 4) value += (precision == 4 ? 1e-6 : 1e-5) * noise.gaussian();
        }
        checks.run(samples, fft, 48000, frequencies, amplitudes, phases, .1, center, "sensitivity", true, precision == 0);
    }
    const std::array frequencies{20.0,28.0}, amplitudes{.25,.08}, phases{.73,-1.1};
    for (const auto kind : {0,1,2,3,4,5,6,7})
    {
        auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, 3);
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            const auto time = static_cast<double>(i) / 48000;
            if (kind < 2) samples[i] = .1 + .25 * std::sin(tau * (80 * time + (kind == 0 ? 8 : 80) * time * time));
            if (kind == 2) samples[i] = .1 + (.05 + .2 * i / 8191) * std::sin(tau * 173 * time);
            if (kind == 3) samples[i] = .1 + .25 * std::sin(tau * (i < 4096 ? 80 : 173) * time);
            if (kind == 4) samples[i] = i == 4096 ? 1 : 0;
            if (kind == 5) samples[i] = .1 * noise.gaussian();
            if (kind == 6) samples[i] += .001 * noise.gaussian();
            if (kind == 7 && i >= 6144) samples[i] = .1 * noise.gaussian();
        }
        checks.run(samples, fft, 48000, {}, {}, {}, 0, 0, "control", false, false);
        if (kind == 4) require(checks.state->guideCount() == 0, "flat impulse spectrum has no numerical-noise guides");
    }
    std::vector<double> samples(8192, .1);
    checks.run(samples, fft, 48000, {}, {}, {}, .1, 0, "constant");
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = .1 + (i % 2 ? -.25 : .25);
    checks.run(samples, fft, 48000, {}, {}, {}, 0, 0, "nyquist", false, false);
    require(checks.state->guideCount() == 0, "Nyquist roundoff cannot create interior guides");
    require(checks.cases == 250, "base source-only inventory");
    for (const auto count : {1024U,2048U,4096U,8192U,16384U,32768U,65536U}) for (const auto rate : {44100.0,48000.0,96000.0})
    {
        const Fft sizeFft(count / 4, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
        const std::array truth{17.3 * rate / count, 27.7 * rate / count};
        samples = waveform(count, rate, truth, amplitudes, phases, .1, .731);
        checks.run(samples, sizeFft, rate, truth, amplitudes, phases, .1, .731, "size_rate");
    }
    samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, 3.141);
    for (auto& value : samples) value *= 1e200;
    checks.run(samples, fft, 48000, frequencies, amplitudes, phases, .1, 3.141, "uniform_scale", true, true, 1e200);
    for (const auto dc : {0.0, -.7})
    { samples.assign(8192, dc); checks.run(samples, fft, 48000, {}, {}, {}, dc, 0, "constant"); }
    for (const auto count : {8192U,65536U}) for (const auto half : {false,true}) for (const auto mixed : {false,true})
    {
        const Fft sizeFft(count / 4, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
        std::array truth{48000.0 * 256 / count * (half ? .5 : 1), 173.2};
        if (mixed) std::sort(truth.begin(), truth.end());
        const auto n = mixed ? 2U : 1U;
        samples = waveform(count, 48000, std::span{truth}.first(n), std::span{amplitudes}.first(n), std::span{phases}.first(n), .1, .731);
        checks.run(samples, sizeFft, 48000, std::span{truth}.first(n), std::span{amplitudes}.first(n), std::span{phases}.first(n), .1, .731, "decimation_boundary", false);
    }
    require(checks.cases == 282, "original profile inventory");
    const std::array<std::vector<double>, 5> aliasBands{{{64,1568},{64,1432},{64,1568,3072},{64,1568,3072,4576},{64,1568,3072,4576,6080}}};
    for (std::size_t band = 0; band < aliasBands.size(); ++band)
    {
        const auto size = aliasBands[band].size();
        std::array<double, 5> amps{}, phs{};
        for (std::size_t i = 0; i < size; ++i) { amps[i] = .25 / (i+1); phs[i] = .37 - .63 * i; }
        for (const auto center : {.731,3.141})
        {
            if (band >= 2 && center != .731) continue;
            samples = waveform(8192, 48000, aliasBands[band], std::span{amps}.first(size), std::span{phs}.first(size), .1, center);
            checks.run(samples, fft, 48000, aliasBands[band], std::span{amps}.first(size), std::span{phs}.first(size), .1, center, "alias_search", band < 4);
            if (band < 4) require(checks.state->attemptedAssignments() >= (band < 2 ? 4U : band == 2 ? 27U : 256U), "Cartesian alias search exercised");
            else require(checks.state->skippedAliasOrders() != 0, "over-cap alias order skipped explicitly");
        }
    }
    checks.report(); require(checks.cases == 289 && checks.accepted == 267 && checks.rejected == 22 && checks.failures == 0, "predeclared source-only profile gate");
}

void testDenseProfiles(bool output)
{
    const Fft fft(2048, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
    Checks checks; checks.output = output;
    for (const auto count : {9U,12U,16U}) for (const auto offBin : {false,true})
    for (const auto descending : {false,true}) for (const auto center : {4096.0/48000,3.141})
    {
        std::vector<double> frequencies(count), amplitudes(count), phases(count);
        for (std::size_t k = 0; k < count; ++k)
        {
            frequencies[k] = offBin ? 91.1 + 37.137*k : 20 + 8*k;
            amplitudes[k] = descending ? .2/(k+1) : .04;
            phases[k] = .37 - .63*k;
        }
        const auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, center);
        checks.run(samples, fft, 48000, frequencies, amplitudes, phases, .1, center,
            offBin ? "dense_off_bin" : "dense_nearby");
        if (!output) std::cout << "dense_model partials=" << count << " off_bin=" << offBin
            << " descending=" << descending << " center=" << center
            << " status=" << static_cast<int>(checks.state->status()) << " size=" << checks.state->size()
            << " guides=" << checks.state->guideCount() << " orders=" << checks.state->attemptedOrders()
            << " assignments=" << checks.state->attemptedAssignments()
            << " work=" << checks.state->consumedWork() << std::endl;
        if (offBin && descending && center == 3.141)
        {
            auto changed = samples;
            for (std::size_t i = 3*changed.size()/4; i < changed.size(); ++i) changed[i] += .02;
            checks.run(changed, fft, 48000, frequencies, amplitudes, phases, .1, center,
                "dense_heldout_change", false);
        }
    }
    checks.report();
    require(checks.cases == 27 && checks.accepted == 24 && checks.rejected == 3 && checks.failures == 0,
        "dense source-only profiles retain discovery, gain, phase and held-out rejection");
}

void same(const Model& a, const Model& b)
{
    require(a.status() == b.status() && a.size() == b.size() && a.dc() == b.dc() && a.consumedWork() == b.consumedWork()
        && a.trainingRelativeRms() == b.trainingRelativeRms() && a.validationRelativeRms() == b.validationRelativeRms()
        && a.attemptedOrders() == b.attemptedOrders() && a.attemptedAssignments() == b.attemptedAssignments()
        && a.skippedAliasOrders() == b.skippedAliasOrders()
        && a.guideCount() == b.guideCount(), "exact model state");
    for (std::size_t i = 0; i < a.size(); ++i)
        require(a.frequency(i) == b.frequency(i) && a.cosineCoefficient(i) == b.cosineCoefficient(i)
            && a.sineCoefficient(i) == b.sineCoefficient(i), "exact fitted components");
    for (std::size_t i = 0; i < a.guideCount(); ++i)
        require(a.guideFrequency(i) == b.guideFrequency(i) && a.guideMagnitude(i) == b.guideMagnitude(i), "exact FFT guides");
}
void testState()
{
    const Fft fft(2048, talas::dsp::Radix2FftSizeProfile::LongAnalysis), wrong(1024, talas::dsp::Radix2FftSizeProfile::LongAnalysis), standard(2048);
    const std::array frequencies{20.0,28.0}, amplitudes{.25,.08}, phases{.73,-1.1};
    auto samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, 3.141);
    auto reference = std::make_unique<Model>(), split = std::make_unique<Model>(), source = std::make_unique<Model>();
    auto work = std::size_t{31}; reference->advance(fft, work);
    require(work == 31 && reference->maximumWork() == 0 && reference->guideCount() == 0, "idle state inert");
    require(reference->begin(samples, 48000), "reference source capture"); finish(*reference, fft);
    require(reference->status() == Model::Status::Ready, "state reference accepted");
    const std::array dirtyFrequencies{317.3,711.7};
    const auto dirtySamples = waveform(8192, 48000, dirtyFrequencies, amplitudes, phases, -.23, .731);
    auto dirty = std::make_unique<Model>();
    require(dirty->begin(dirtySamples, 48000), "unlike dirty checkpoint capture"); finish(*dirty, fft);
    for (const auto partition : {1U,97U,65536U})
    {
        require(split->begin(samples, 48000), "partition capture"); work = 0; split->advance(wrong, work);
        require(split->consumedWork() == 0 && split->status() == Model::Status::Running && split->size() == 0 && split->dc() == 0
            && split->guideCount() == 0 && split->frequency(0) == 0, "zero work inert and pending output hidden");
        finish(*split, fft, partition); same(*reference, *split);
    }
    std::size_t copies = 0;
    for (const auto position : std::array<std::uint64_t, 16>{1,8191,8192,14335,14336,20479,20480,24000,40000,70000,
        100000ULL,reference->consumedWork()/4,reference->consumedWork()/2,3*reference->consumedWork()/4,
        9*reference->consumedWork()/10,reference->consumedWork()-1})
    {
        require(source->begin(samples, 48000), "checkpoint capture"); work = static_cast<std::size_t>(position); source->advance(fft, work);
        if (source->status() != Model::Status::Running) continue;
        *split = *dirty;
        noAllocation([&] { split->copyProcessingStateFrom(*source); source->reset(); return true; });
        finish(*split, fft, 701); same(*reference, *split); ++copies;
    }
    require(copies >= 14, "multiple FFT/solver/fit pending copies");
    require(split->begin(samples, 48000), "owned source capture"); std::fill(samples.begin(), samples.end(), std::numeric_limits<double>::quiet_NaN());
    finish(*split, fft); same(*reference, *split);
    require(split->begin(samples, 48000), "nonfinite raw admission"); finish(*split, fft);
    require(split->status() == Model::Status::InvalidInput && split->size() == 0 && split->dc() == 0, "nonfinite sample rejected");
    samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, 3.141);
    require(split->begin(samples, 48000), "wrong FFT capture"); work = 1; split->advance(wrong, work);
    require(split->status() == Model::Status::InvalidInput && work == 1, "wrong FFT plan rejected");
    require(split->begin(samples, 48000), "FFT profile capture"); work = 20480; split->advance(fft, work);
    require(split->status() == Model::Status::Running && work == 0, "pending FFT cursor prepared");
    work = 1; split->advance(standard, work);
    require(split->status() == Model::Status::InvalidInput && work == 1, "FFT profile cannot change inside transform");
    require(!split->begin({}, 48000) && !split->begin(std::span{samples}.first(1023), 48000)
        && !split->begin(std::span{samples}.first(3000), 48000), "invalid sample dimensions");
    const std::vector<double> oversized(65537);
    require(!split->begin(oversized, 48000), "sample capacity closed");
    for (const auto rate : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        require(!split->begin(samples, rate), "invalid sample rate");
    samples[0] = 1e300; samples[1] = 1e-300;
    require(split->begin(samples, 48000), "range-loss admission"); finish(*split, fft);
    require(split->status() == Model::Status::NumericFailure && split->dc() == 0, "range loss fails closed");
    samples = waveform(8192, 48000, frequencies, amplitudes, phases, .1, 3.141);
    for (auto& value : samples) value *= 1e-200;
    require(split->begin(samples, 48000), "sub-floor source capture"); finish(*split, fft);
    require(split->status() == Model::Status::Ready && split->size() == 0 && std::abs(split->dc()) < 1e-12, "physical floor permits constant approximation");
    noAllocation([&] { split->reset(); return true; });
    require(split->status() == Model::Status::Idle && split->guideCount() == 0 && split->maximumWork() == 0 && split->frequency(0) == 0, "scalar reset hides data");
    std::cout << "checkpoint_positions=" << copies << " sizeof_model=" << sizeof(Model) << " reference_work=" << reference->consumedWork() << std::endl;
}

void testStageAwareCopies()
{
    using Access = talas::dsp::IncrementalStationaryModelTestAccess;
    const std::array dirtyFrequencies{317.3,711.7}, dirtyAmplitudes{.33,.07}, dirtyPhases{.49,.93};
    const auto dirtySamples = waveform(65536, 48000, dirtyFrequencies, dirtyAmplitudes, dirtyPhases, -.23, .731);
    const Fft longFft(16384, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
    auto dirty = std::make_unique<Model>();
    require(dirty->begin(dirtySamples, 48000), "largest unlike dirty model capture"); finish(*dirty, longFft);
    require(dirty->status() == Model::Status::Ready, "dirty model fully populates unlike solver and FFT storage");
    const std::array frequencies{91.1,171.3,251.9,332.3,411.7,493.1,573.7,653.9};
    std::array<double, 8> amplitudes{}, phases{};
    for (std::size_t i = 0; i < frequencies.size(); ++i) { amplitudes[i] = .25 / (i+1); phases[i] = .37 - .63 * i; }
    std::uint32_t visitedStages = 0;
    std::size_t totalCopies = 0;
    std::size_t penultimateRetries = 0, lastAccepted = 0, lastRejected = 0, exhaustedCopies = 0;
    std::size_t constantAccepted = 0, constantRejected = 0;
    for (const auto fixture : {0,1,2,3,4,5,6,7,8})
    {
        const auto count = fixture == 1 || fixture == 6 ? std::size_t{65536} : std::size_t{8192};
        const auto partials = fixture == 0 || fixture == 4 || fixture == 6 || fixture == 7 ? std::size_t{8} : std::size_t{2};
        auto sourceFrequencies = frequencies;
        auto sourceAmplitudes = amplitudes, sourcePhases = phases;
        // A unique assignment now goes directly to a validated final fit.
        // Retain explicit multi-assignment training-only Fit checkpoints.
        if (fixture == 8) { sourceFrequencies[0] = 64; sourceFrequencies[1] = 1568; }
        if (fixture == 6) for (auto& frequency : sourceFrequencies) frequency *= 8192.0 / count;
        // Independent seven-tone least squares gives training9.68241e-5 and
        // held-out1.08249e-4 relative RMS. The actual estimator must reject its
        // order14 final fit, then recover the weak eighth tone at order16.
        if (fixture == 7)
        {
            sourceFrequencies[7] = 20.3;
            sourceAmplitudes[7] = 3.1e-5;
            sourcePhases[7] = 5.779335406794897;
        }
        auto samples = waveform(count, 48000, std::span(sourceFrequencies).first(partials),
            std::span(sourceAmplitudes).first(partials), std::span(sourcePhases).first(partials), .1, 3.141);
        if (fixture == 2 || fixture == 5) std::fill(samples.begin(), samples.end(), .17);
        if (fixture == 3) { std::fill(samples.begin(), samples.end(), 0); samples[count/2] = 1; }
        // Preserve the training signal but contradict it in the held-out tail.
        if (fixture == 4 || fixture == 5)
            for (std::size_t i = 3 * count / 4; i < count; ++i) samples[i] += .2;
        const Fft fft(count/4, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
        auto source = std::make_unique<Model>();
        require(source->begin(samples, 48000), "stage copy source capture");
        std::array<std::size_t, 64> visits{};
        std::array<bool, 128> captured{};
        std::vector<std::unique_ptr<Model>> checkpoints;
        while (source->status() == Model::Status::Running)
        {
            visitedStages |= 1U << Access::stage(*source);
            const auto base = Access::bucket(*source);
            const auto bucket = 2 * base + (++visits[base] >= 97 ? 1 : 0);
            if (!captured[bucket] && (fixture != 6 || Access::terminalFinalFit(*source)))
            {
                auto copy = std::make_unique<Model>(*dirty);
                noAllocation([&] { copy->copyProcessingStateFrom(*source); return true; });
                same(*source, *copy);
                checkpoints.push_back(std::move(copy)); captured[bucket] = true;
            }
            auto work = std::size_t{1}; source->advance(fft, work);
            require(work == 0 && source->consumedWork() <= source->maximumWork(), "stage coverage makes bounded progress");
        }
        const auto reject = fixture == 3 || fixture == 4 || fixture == 5;
        require(source->status() == (reject ? Model::Status::Rejected : Model::Status::Ready),
            "stage copy fixture reaches its independently expected acceptance class");
        const auto expected = std::make_unique<Model>(*source);
        // Destroy the source trajectory before resuming its snapshots. A
        // checkpoint must own the raw samples and every live nested workspace.
        require(source->begin(dirtySamples, 48000), "replace original source after checkpoint"); finish(*source, longFft);
        for (auto& copy : checkpoints)
        {
            const auto order = Access::finalOrder(*copy), previousOrders = static_cast<int>(copy->attemptedOrders());
            const auto last = Access::terminalFinalFit(*copy), penultimate = Access::penultimateFinalFit(*copy);
            if (last && reject)
            {
                // Stop exactly after a rejected final fit exhausts its unit.
                // Copy again after its parent buffers were already omitted.
                noAllocation([&] {
                    while (copy->status() == Model::Status::Running && !Access::exhaustedOrder(*copy))
                    {
                        auto work = std::size_t{1}; copy->advance(fft, work);
                        require(work == 0 && copy->consumedWork() <= copy->maximumWork(),
                            "final rejection boundary makes bounded progress");
                    }
                    return true;
                });
                require(copy->status() == Model::Status::Running && Access::exhaustedOrder(*copy),
                    "last-order failure exposes the exact exhausted-work rejection boundary");
                auto boundaryCopy = std::make_unique<Model>(*dirty);
                noAllocation([&] { boundaryCopy->copyProcessingStateFrom(*copy); return true; });
                finish(*boundaryCopy, fft, 701); same(*expected, *boundaryCopy); ++exhaustedCopies;
            }
            finish(*copy, fft, 701); same(*expected, *copy);
            if (penultimate && !reject)
            {
                const auto retried = static_cast<int>(copy->attemptedOrders()) > previousOrders;
                // Fixture 7 independently requires a rejected seven-tone fit.
                // Other inputs may validly finish before their retained rank
                // is exhausted, including the multi-alias fixture above.
                if (fixture == 7)
                    require(retried,
                        "penultimate final fit must retain raw/subspace state for the successful next order");
                if (retried) ++penultimateRetries;
            }
            if (last) ++(reject ? lastRejected : lastAccepted);
            if (order == 0) ++(reject ? constantRejected : constantAccepted);
        }
        auto terminal = std::make_unique<Model>(*dirty);
        noAllocation([&] { terminal->copyProcessingStateFrom(*expected); return true; });
        same(*expected, *terminal);
        noAllocation([&] { terminal->copyProcessingStateFrom(*terminal); return true; });
        same(*expected, *terminal);
        totalCopies += checkpoints.size();
        std::cout << "model_stage_copy fixture=" << fixture << " samples=" << count
            << " checkpoints=" << checkpoints.size() << " terminal=1 noalloc=1\n";
    }
    require(visitedStages == 0xffffU, "all sixteen parent stages are exercised by semantic copies");
    require(penultimateRetries >= 2 && lastAccepted >= 4 && lastRejected >= 2 && exhaustedCopies >= 2
        && constantAccepted >= 2 && constantRejected >= 2, "last-attempt copy boundaries and earlier retry paths are covered");
    auto terminal = std::make_unique<Model>(), copy = std::make_unique<Model>(*dirty);
    const auto compareTerminal = [&] {
        noAllocation([&] { copy->copyProcessingStateFrom(*terminal); return true; }); same(*terminal, *copy);
    };
    compareTerminal();
    require(!terminal->begin({}, 48000), "invalid terminal fixture"); compareTerminal();
    auto rangeLoss = dirtySamples; rangeLoss[0] = 1e300; rangeLoss[1] = 1e-300;
    require(terminal->begin(rangeLoss, 48000), "numeric terminal fixture"); finish(*terminal, longFft);
    require(terminal->status() == Model::Status::NumericFailure, "numeric terminal rejected"); compareTerminal();
    std::cout << "model_stage_copies=" << totalCopies << " stages=16 terminal_classes=5\n";
    std::cout << "model_final_copies penultimate_retries=" << penultimateRetries << " accepted=" << lastAccepted
        << " rejected=" << lastRejected << " exhausted_boundaries=" << exhaustedCopies
        << " constant_accepted=" << constantAccepted << " constant_rejected=" << constantRejected << '\n';
}

void probeBudget()
{
    for (const auto fixture : {0,1,2})
    {
        const auto count = fixture == 2 ? std::size_t{65536} : std::size_t{8192};
        const auto partials = fixture == 1 ? std::size_t{8} : std::size_t{2};
        const std::array frequencies{20.0,28.0,171.3,251.9,332.3,411.7,493.1,573.7};
        std::array<double, 8> amplitudes{}, phases{};
        for (std::size_t i = 0; i < partials; ++i) { amplitudes[i] = .25 / (i+1); phases[i] = .73 - 1.83 * i; }
        const auto samples = waveform(count, 48000, std::span{frequencies}.first(partials),
            std::span{amplitudes}.first(partials), std::span{phases}.first(partials), .1, 3.141);
        const Fft fft(count/4, talas::dsp::Radix2FftSizeProfile::LongAnalysis);
        auto prepared = std::make_unique<Model>(), state = std::make_unique<Model>();
        const auto captureStart = std::chrono::steady_clock::now();
        require(prepared->begin(samples, 48000), "timing capture");
        const auto captureUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - captureStart).count();
        for (const auto partition : {4096U,16384U})
        {
            *state = *prepared;
            // Storage, checkpoint copying and sorting are outside timed calls.
            std::vector<double> durations; durations.reserve(100000);
            while (state->status() == Model::Status::Running)
            {
                auto work = static_cast<std::size_t>(partition);
                const auto start = std::chrono::steady_clock::now(); state->advance(fft, work);
                durations.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count());
                require(state->consumedWork() <= state->maximumWork(), "timed work bound");
            }
            require(state->status() == Model::Status::Ready, "timing model accepted");
            std::sort(durations.begin(), durations.end());
            std::cout << "model_budget samples=" << count << " partials=" << partials << " budget=" << partition
                << " capture_us=" << captureUs << " calls=" << durations.size() << " work=" << state->consumedWork()
                << " p99_us=" << durations[static_cast<std::size_t>((durations.size()-1) * .99)] << " max_us=" << durations.back() << std::endl;
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--budget-probe") { probeBudget(); return 0; }
        if (argc == 2 && std::string_view{argv[1]} == "--copy-probe") { testStageAwareCopies(); return 0; }
        if (argc == 2 && (std::string_view{argv[1]} == "--dense-probe" || std::string_view{argv[1]} == "--dense-dump"))
        { testDenseProfiles(std::string_view{argv[1]} == "--dense-dump"); return 0; }
        const auto output = argc == 2 && std::string_view{argv[1]} == "--dump";
        const auto stress = argc == 2 && std::string_view{argv[1]} == "--stress";
        testProfiles(output, stress);
        if (!output && !stress) { testState(); testStageAwareCopies(); std::cout << "Incremental stationary model tests passed" << std::endl; }
        return 0;
    }
    catch (const std::exception& error)
    { counting = false; std::cerr << "Incremental stationary model tests failed: " << error.what() << std::endl; return 1; }
}
