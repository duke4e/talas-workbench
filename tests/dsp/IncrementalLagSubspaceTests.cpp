#include "talas/dsp/IncrementalLagSubspace.h"
#include "../support/LagSubspaceStationaryReference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <type_traits>

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

namespace
{
using Subspace = talas::dsp::IncrementalLagSubspace;
using Status = Subspace::Status;
constexpr auto tau = 2 * std::numbers::pi;
static_assert(std::is_trivially_copyable_v<Subspace>);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void finish(Subspace& state, std::size_t partition = 4096)
{
    const auto bound = state.maximumWork();
    std::size_t previous = state.consumedWork();
    while (state.status() == Status::Running)
    {
        auto budget = partition;
        allocations = 0;
        countAllocations = true;
        state.advance(budget);
        countAllocations = false;
        require(allocations == 0, "subspace advance must allocate nothing");
        require(budget <= partition && state.consumedWork() - previous == partition - budget,
                "every row operation must consume exactly one supplied work unit");
        require(state.consumedWork() > previous && state.consumedWork() <= bound,
                "pending subspace work must advance within its declared bound");
        previous = state.consumedWork();
    }
}

void same(const Subspace& first, const Subspace& second)
{
    require(first.status() == second.status() && first.rank() == second.rank()
            && first.rows() == second.rows() && first.columns() == second.columns()
            && first.consumedWork() == second.consumedWork() && first.sweeps() == second.sweeps(),
            "partition/checkpoint metadata must match exactly");
    for (std::size_t mode = 0; mode < first.rank(); ++mode)
    {
        require(first.singularValue(mode) == second.singularValue(mode), "checkpoint singular values");
        for (std::size_t row = 0; row < first.rows(); ++row)
            require(first.basis(mode, row) == second.basis(mode, row), "checkpoint temporal basis");
    }
}

using talas::test::lag_subspace::sourceDifferences;

double projectionError(const Subspace& state, std::size_t modes, double frequency, bool sine)
{
    std::array<double, Subspace::maximumSequenceSize> ideal{}, residual{};
    double energy = 0;
    for (std::size_t row = 0; row < state.rows(); ++row)
    {
        const auto phase = tau * frequency * row / 1500;
        residual[row] = ideal[row] = sine ? std::sin(phase) : std::cos(phase);
        energy += ideal[row] * ideal[row];
    }
    for (std::size_t mode = 0; mode < modes; ++mode)
    {
        double dot = 0;
        for (std::size_t row = 0; row < state.rows(); ++row) dot += ideal[row] * state.basis(mode, row);
        for (std::size_t row = 0; row < state.rows(); ++row) residual[row] -= dot * state.basis(mode, row);
    }
    double error = 0;
    for (std::size_t row = 0; row < state.rows(); ++row) error += residual[row] * residual[row];
    return std::sqrt(error / energy);
}

void verifyBasis(const Subspace& state, std::span<const double> sequence,
                 std::span<const double> frequencies, bool quantized)
{
    require(state.status() == Status::Ready, "stationary lag subspace must converge");
    const auto modes = 2 * frequencies.size();
    require(state.rank() >= modes, "retain both real modes of every sinusoid");
    if (!quantized) require(state.rank() == modes, "clean mixtures must not expose spurious tail modes");
    double expectedEnergy = 0, actualEnergy = 0;
    for (std::size_t column = 0; column < state.columns(); ++column)
        for (std::size_t row = 0; row < state.rows(); ++row)
            expectedEnergy += sequence[row + column] * sequence[row + column];
    for (std::size_t mode = 0; mode < state.rank(); ++mode)
        actualEnergy += state.singularValue(mode) * state.singularValue(mode);
    require(std::abs(actualEnergy / expectedEnergy - 1) < 1e-10, "orthogonal rotations preserve matrix energy");
    for (std::size_t first = 0; first < modes; ++first)
        for (std::size_t second = 0; second < modes; ++second)
        {
            double dot = 0;
            for (std::size_t row = 0; row < state.rows(); ++row)
                dot += state.basis(first, row) * state.basis(second, row);
            require(std::abs(dot - (first == second ? 1 : 0)) < 1e-10, "dominant basis must be orthonormal");
        }
    for (const auto frequency : frequencies)
        for (const auto sine : {false, true})
            require(projectionError(state, modes, frequency, sine) < (quantized ? .005 : 1e-8),
                    "independent sinusoid columns must lie in the estimated temporal subspace");
}

void dump(const Subspace& state, std::span<const double> sequence, std::size_t modes,
          std::string_view name)
{
    std::cout << std::setprecision(17) << "{\"name\":\"" << name << "\",\"rows\":" << state.rows()
              << ",\"columns\":" << state.columns() << ",\"modes\":" << modes
              << ",\"rank\":" << state.rank() << ",\"sequence\":[";
    for (std::size_t i = 0; i < sequence.size(); ++i) std::cout << (i ? "," : "") << sequence[i];
    std::cout << "],\"singular_values\":[";
    for (std::size_t i = 0; i < modes; ++i) std::cout << (i ? "," : "") << state.singularValue(i);
    std::cout << "],\"basis\":[";
    for (std::size_t mode = 0; mode < modes; ++mode)
    {
        std::cout << (mode ? ",[" : "[");
        for (std::size_t row = 0; row < state.rows(); ++row) std::cout << (row ? "," : "") << state.basis(mode, row);
        std::cout << ']';
    }
    std::cout << "]}" << std::endl;
}

void testSmallMatricesAndAdmission()
{
    Subspace state;
    auto budget = std::size_t{7};
    state.advance(budget);
    require(budget == 7 && state.status() == Status::Idle, "idle advance is inert");
    require(!state.begin({}, 1), "empty sequence rejected");
    const std::array invalid{0.0, std::numeric_limits<double>::quiet_NaN(), 1.0};
    require(!state.begin(invalid, 2), "nonfinite snapshot rejected");
    const std::array simple{1.0, 2.0, 3.0};
    require(!state.begin(simple, 3) && !state.begin(simple, 0), "lag dimensions are closed");
    require(state.begin(simple, 2), "small valid lag matrix");
    finish(state, 1);
    require(state.status() == Status::Ready && state.rank() == 2, "full rank small matrix");
    require(std::abs(state.singularValue(0) - (2 + std::sqrt(5.0))) < 1e-12
            && std::abs(state.singularValue(1) - (std::sqrt(5.0) - 2)) < 1e-12,
            "independently known singular values of the symmetric two-by-two matrix");
    const std::array column{3.0, 4.0};
    require(state.begin(column, 1), "single column admission");
    finish(state, 7);
    require(state.rank() == 1 && state.singularValue(0) == 5
            && state.basis(0, 0) == .6 && state.basis(0, 1) == .8, "single column exact norm");
    const std::array zeros{0.0, 0.0, 0.0};
    require(state.begin(zeros, 2) && state.status() == Status::Ready && state.rank() == 0
            && state.basis(0, 0) == 0 && state.singularValue(0) == 0, "zero input invalidates old modes");
    const auto large = std::numeric_limits<double>::max();
    const std::array overflow{large, large, large};
    require(state.begin(overflow, 2), "large finite input captured safely");
    finish(state);
    require(state.status() == Status::NumericOverflow && state.rank() == 0, "overflow exposes no partial basis");
    state.reset();
    require(state.status() == Status::Idle && state.rank() == 0 && state.maximumWork() == 0,
            "reset invalidates all visible state");
}

void testPartitionCheckpointAndCapture()
{
    const std::array frequencies{20.0, 28.0}, amplitudes{.25, .08};
    auto sequence = sourceDifferences(frequencies, amplitudes);
    Subspace reference;
    require(reference.begin(sequence, 95), "reference capture");
    finish(reference, 4096);
    require(reference.status() == Status::Ready, "reference convergence");
    for (const auto partition : {std::size_t{97}, std::size_t{4093}, std::size_t{65536}})
    {
        Subspace current;
        require(current.begin(sequence, 95), "partition capture");
        auto zero = std::size_t{0};
        current.advance(zero);
        require(current.consumedWork() == 0, "zero budget is inert");
        finish(current, partition);
        same(reference, current);
    }
    std::size_t copies = 0;
    for (const auto offset : {1U, 9214U, 9215U, 9312U, 9313U, 9410U, 20000U, 500000U, 1000000U})
    {
        Subspace source, destination = reference;
        require(source.begin(sequence, 95), "pending capture");
        auto work = static_cast<std::size_t>(offset);
        source.advance(work);
        if (source.status() != Status::Running) continue;
        allocations = 0;
        countAllocations = true;
        destination = source;
        countAllocations = false;
        require(allocations == 0, "checkpoint copy must allocate nothing");
        source.reset();
        finish(destination, 701);
        same(reference, destination);
        ++copies;
    }
    require(copies >= 7, "exercise several pending positions and dirty destination copies");
    Subspace captured;
    require(captured.begin(sequence, 95), "capture ownership fixture");
    sequence.fill(std::numeric_limits<double>::quiet_NaN());
    finish(captured);
    same(reference, captured);
    allocations = 0;
    countAllocations = true;
    captured.reset();
    countAllocations = false;
    require(allocations == 0, "reset must allocate nothing");
    std::cout << "checkpoint_positions=" << copies << " sizeof_state=" << sizeof(Subspace) << std::endl;
}

void testNoiseDimensionsAndScaling()
{
    std::array<double, Subspace::maximumSequenceSize + 1> oversized{};
    Subspace state;
    require(!state.begin(oversized, 95), "oversized sequence rejected before capture");
    std::array<double, Subspace::maximumSequenceSize> sequence{};
    std::uint32_t seed = 271828;
    for (auto& value : sequence)
    {
        seed = 1664525U * seed + 1013904223U;
        value = static_cast<double>(seed) / 4294967296.0 - .5;
    }
    require(!state.begin(sequence, 96), "column bound is closed");
    for (const auto columns : {1U, 2U, 8U, 95U})
    {
        require(state.begin(sequence, columns), "admitted boundary dimensions");
        finish(state, 997);
        require(state.status() == Status::Ready && state.rank() == columns,
                "deterministic full-rank noise converges without manufacturing a low rank");
        require(state.singularValue(columns) == 0 && state.basis(0, state.rows()) == 0,
                "out-of-range results are inaccessible");
        for (std::size_t i = 0; i < state.rank(); ++i)
            for (std::size_t j = 0; j < state.rank(); ++j)
            {
                double dot = 0;
                for (std::size_t row = 0; row < state.rows(); ++row)
                    dot += state.basis(i, row) * state.basis(j, row);
                require(std::abs(dot - (i == j ? 1 : 0)) < 1e-10, "full-rank noise orthogonality");
            }
    }
    // Matrix normalization must avoid both squared-norm overflow and underflow.
    Subspace reference;
    require(reference.begin(sequence, 8), "scaling reference");
    finish(reference);
    for (const auto scale : {1e-200, 1e200})
    {
        auto scaled = sequence;
        for (auto& value : scaled) value *= scale;
        require(state.begin(scaled, 8), "finite extreme scale admission");
        finish(state);
        require(state.status() == Status::Ready && state.rank() == reference.rank(), "scale-independent rank");
        for (std::size_t mode = 0; mode < reference.rank(); ++mode)
            require(std::abs(state.singularValue(mode) / scale / reference.singularValue(mode) - 1) < 1e-10,
                    "rescaled singular values retain physical units");
    }
}

void testStationarySources(bool dumpResults)
{
    std::size_t cases = 0, maximumWork = 0, maximumSweeps = 0;
    double maximumMilliseconds = 0, maximumProjection = 0;
    auto run = [&](std::span<const double> frequencies, std::span<const double> amplitudes,
                   bool quantized, double center, std::string_view name)
    {
        const auto sequence = sourceDifferences(frequencies, amplitudes, quantized, center);
        Subspace state;
        allocations = 0;
        countAllocations = true;
        const auto captured = state.begin(sequence, 95);
        countAllocations = false;
        require(captured && allocations == 0, "source capture must allocate nothing");
        const auto start = std::chrono::steady_clock::now();
        finish(state);
        const auto milliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (state.status() != Status::Ready)
            std::cerr << "nonconvergence: " << name << " status=" << static_cast<int>(state.status())
                      << " sweeps=" << state.sweeps() << " work=" << state.consumedWork() << std::endl;
        verifyBasis(state, sequence, frequencies, quantized);
        maximumWork = std::max(maximumWork, state.consumedWork());
        maximumSweeps = std::max(maximumSweeps, state.sweeps());
        maximumMilliseconds = std::max(maximumMilliseconds, milliseconds);
        for (const auto frequency : frequencies)
            for (const auto sine : {false, true})
                maximumProjection = std::max(maximumProjection,
                    projectionError(state, 2 * frequencies.size(), frequency, sine));
        ++cases;
        if (dumpResults) dump(state, sequence, 2 * frequencies.size(), name);
    };
    talas::test::lag_subspace::forEachProfile(run);
    if (!dumpResults)
        std::cout << "stationary_cases=" << cases << " max_work=" << maximumWork
                  << " max_sweeps=" << maximumSweeps << " max_projection_error=" << maximumProjection
                  << " max_whole_job_ms=" << maximumMilliseconds << std::endl;
}

void testWeakModeSpreadAcrossColumns()
{
    // Each residual column is below the rank floor, but together they contain
    // two singular modes above it. Over 97 rows the DC, sine and cosine vectors
    // are orthogonal, giving an independent approximation to the weak singular
    // values. Its O(amplitude squared) correction is below source rounding.
    constexpr double amplitude = 4e-12;
    constexpr std::size_t rows = 97, columns = 95;
    const auto angle = tau * 7 / rows;
    std::array<double, Subspace::maximumSequenceSize> sequence{};
    for (std::size_t i = 0; i < sequence.size(); ++i)
        sequence[i] = 1 + amplitude * std::sin(angle * i);
    Subspace state;
    require(state.begin(sequence, columns), "weak distributed mode capture");
    finish(state, 997);
    require(state.status() == Status::Ready && state.rank() == 3,
            "subthreshold columns must combine into all above-threshold modes");
    // Removing the column mean couples the small modes to the dominant DC
    // direction. Compute the remaining two-by-two Gram spectrum directly.
    double sineSum = 0, cosineSum = 0, sineSquared = 0, cosineSquared = 0, cross = 0;
    for (std::size_t i = 0; i < columns; ++i)
    {
        const auto sine = std::sin(angle * i), cosine = std::cos(angle * i);
        sineSum += sine;
        cosineSum += cosine;
        sineSquared += sine * sine;
        cosineSquared += cosine * cosine;
        cross += sine * cosine;
    }
    sineSquared -= sineSum * sineSum / columns;
    cosineSquared -= cosineSum * cosineSum / columns;
    cross -= sineSum * cosineSum / columns;
    const auto midpoint = (sineSquared + cosineSquared) / 2;
    const auto radius = std::hypot((sineSquared - cosineSquared) / 2, cross);
    for (std::size_t mode = 1; mode <= 2; ++mode)
    {
        const auto expected = amplitude * std::sqrt(rows * .5
            * (midpoint + (mode == 1 ? radius : -radius)));
        require(std::abs(state.singularValue(mode) / expected - 1) < .001,
                "distributed weak singular values agree with independent Gram spectrum");
    }
    for (std::size_t i = 0; i < sequence.size(); ++i)
        sequence[i] = 1 + amplitude * .1 * std::sin(angle * i);
    require(state.begin(sequence, columns), "below-floor distributed mode capture");
    finish(state, 997);
    require(state.status() == Status::Ready && state.rank() == 1,
            "better internal orthogonalization must retain the original rank floor");
}

void probeWorkPartitions()
{
    const std::array frequencies{20.0, 28.0}, amplitudes{.25, .08};
    const auto stationary = sourceDifferences(frequencies, amplitudes, true);
    auto noise = stationary;
    std::uint32_t seed = 314159;
    for (auto& value : noise)
    {
        seed = 1664525U * seed + 1013904223U;
        value = static_cast<double>(seed) / 4294967296.0 - .5;
    }
    for (const auto noisy : {false, true})
        for (const auto partition : {4096U, 8192U, 16384U, 32768U})
        {
            Subspace state;
            require(state.begin(noisy ? noise : stationary, 95), "work partition probe capture");
            // Storage is prepared outside every timed advance call.
            std::array<double, 6000> durations{};
            std::size_t calls = 0;
            while (state.status() == Status::Running)
            {
                require(calls < durations.size(), "profile capacity covers the declared work bound");
                auto work = static_cast<std::size_t>(partition);
                const auto start = std::chrono::steady_clock::now();
                state.advance(work);
                durations[calls++] = std::chrono::duration<double, std::micro>(
                    std::chrono::steady_clock::now() - start).count();
                require(state.consumedWork() <= state.maximumWork(), "profile preserves work bound");
            }
            require(state.status() == Status::Ready, "profile job converges");
            std::sort(durations.begin(), durations.begin() + static_cast<std::ptrdiff_t>(calls));
            std::cout << "primitive_budget noisy=" << noisy << " budget=" << partition
                      << " calls=" << calls << " work=" << state.consumedWork()
                      << " p99_us=" << durations[static_cast<std::size_t>((calls - 1) * .99)]
                      << " max_us=" << durations[calls - 1] << std::endl;
        }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--budget-probe")
        { probeWorkPartitions(); return 0; }
        const auto dumpResults = argc == 2 && std::string_view{argv[1]} == "--dump";
        if (!dumpResults)
        {
            testSmallMatricesAndAdmission();
            testPartitionCheckpointAndCapture();
            testNoiseDimensionsAndScaling();
            testWeakModeSpreadAcrossColumns();
        }
        testStationarySources(dumpResults);
        if (!dumpResults) std::cout << "Incremental lag subspace tests passed" << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << "Incremental lag subspace tests failed: " << error.what() << std::endl;
        return 1;
    }
}
