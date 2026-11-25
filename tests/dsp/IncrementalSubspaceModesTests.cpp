#include "talas/dsp/IncrementalSubspaceModes.h"
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
#include <numbers>
#include <stdexcept>
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
using Eigen = talas::dsp::IncrementalComplexEigenvalues;
using Modes = talas::dsp::IncrementalSubspaceModes;
using Subspace = talas::dsp::IncrementalLagSubspace;
using Complex = Eigen::Complex;
using Matrix = std::array<Complex, Eigen::maximumSize * Eigen::maximumSize>;
using Roots = std::array<Complex, Eigen::maximumSize>;
constexpr auto tau = 2 * std::numbers::pi;
static_assert(std::is_trivially_copyable_v<Eigen> && std::is_trivially_copyable_v<Modes>);

void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }

template <class Action> auto withoutAllocations(Action&& action)
{
    allocations = 0;
    countAllocations = true;
    const auto result = action();
    countAllocations = false;
    require(allocations == 0, "capture/advance/copy/reset must allocate nothing");
    return result;
}

template <class State> void finish(State& state, std::size_t partition = 4096)
{
    auto previous = state.consumedWork();
    while (state.status() == State::Status::Running)
    {
        auto work = partition;
        withoutAllocations([&] { state.advance(work); return true; });
        require(work <= partition && state.consumedWork() - previous == partition - work,
                "work consumed exactly matches the budget");
        require(state.consumedWork() > previous && state.consumedWork() <= state.maximumWork(),
                "pending work progresses within the explicit bound");
        previous = state.consumedWork();
    }
}

template <class State> double match(const State& state, std::span<const Complex> expected)
{
    require(state.status() == State::Status::Ready && state.size() == expected.size(), "complete root set");
    std::array<bool, Eigen::maximumSize> used{};
    double error = 0, scale = 1;
    for (const auto root : expected) scale = std::max(scale, std::abs(root));
    for (const auto root : expected)
    {
        auto nearest = expected.size();
        double distance = std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!used[i] && std::abs(state.eigenvalue(i) - root) < distance)
            { nearest = i; distance = std::abs(state.eigenvalue(i) - root); }
        require(nearest < expected.size(), "finite unordered roots");
        used[nearest] = true;
        error = std::max(error, distance / scale);
    }
    return error;
}

template <class State> void same(const State& first, const State& second)
{
    require(first.status() == second.status() && first.size() == second.size()
            && first.consumedWork() == second.consumedWork(), "exact checkpoint metadata");
    for (std::size_t i = 0; i < first.size(); ++i)
        require(first.eigenvalue(i) == second.eigenvalue(i), "exact checkpoint roots");
}

void complexJson(Complex value) { std::cout << '[' << value.real() << ',' << value.imag() << ']'; }
template <class State> void rootsJson(const State& state)
{
    std::cout << ",\"roots\":[";
    for (std::size_t i = 0; i < state.size(); ++i)
    { if (i) std::cout << ','; complexJson(state.eigenvalue(i)); }
    std::cout << ']';
}
void dumpEigen(const Eigen& state, std::span<const Complex> matrix, std::string_view name)
{
    std::cout << std::setprecision(17) << "{\"kind\":\"eigen\",\"name\":\"" << name
              << "\",\"size\":" << state.size() << ",\"matrix\":[";
    for (std::size_t i = 0; i < matrix.size(); ++i)
    { if (i) std::cout << ','; complexJson(matrix[i]); }
    std::cout << ']';
    rootsJson(state);
    std::cout << "}" << std::endl;
}

Matrix complexSimilarity(std::size_t n, Roots& expected)
{
    Matrix matrix{};
    for (std::size_t k = 0; k < n; ++k)
        expected[k] = std::polar(.6 + .02 * k, .13 + tau * k / n);
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j)
        {
            for (std::size_t k = 0; k < n; ++k)
                matrix[i * n + j] += expected[k] * std::polar(1.0 / n,
                    tau * (static_cast<double>(i) - j) * k / n);
            matrix[i * n + j] *= (1.0 + i / 3.0) / (1.0 + j / 3.0);
        }
    return matrix;
}

std::array<double, Subspace::maximumSequenceSize> maximumModeSequence()
{
    constexpr std::size_t partialCount = Eigen::maximumSize / 2;
    std::array<double, partialCount> frequencies{}, amplitudes{};
    for (std::size_t i = 0; i < partialCount; ++i)
    {
        frequencies[i] = 45.0 + 37.0 * i;
        amplitudes[i] = .1;
    }
    auto sample = [&](std::size_t index)
    {
        const auto time = 3.141 + (static_cast<double>(index * 32) - 4096) / 48000;
        auto value = .1;
        for (std::size_t i = 0; i < partialCount; ++i)
            value += amplitudes[i] * std::sin(tau * frequencies[i] * time + .73 - 1.83 * i);
        return value;
    };
    std::array<double, Subspace::maximumSequenceSize> differences{};
    auto previous = sample(0);
    for (std::size_t i = 0; i < differences.size(); ++i)
    {
        const auto next = sample(i + 1);
        differences[i] = next - previous;
        previous = next;
    }
    return differences;
}

void testEigenFixtures(bool dump)
{
    std::size_t cases = 0, analytic = 0, maximumWork = 0, maximumIterations = 0;
    double maximumError = 0, maximumMs = 0;
    auto run = [&](const Matrix& matrix, std::size_t n, const Roots& expected, bool known, std::string_view name)
    {
        Eigen state;
        require(withoutAllocations([&] { return state.begin(std::span{matrix}.first(n * n), n); }), "eigen capture");
        const auto start = std::chrono::steady_clock::now();
        finish(state);
        maximumMs = std::max(maximumMs, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
        if (state.status() != Eigen::Status::Ready)
            std::cerr << "nonconvergence: " << name << " n=" << n << " status=" << static_cast<int>(state.status())
                      << " iterations=" << state.iterations() << " work=" << state.consumedWork() << std::endl;
        require(state.status() == Eigen::Status::Ready, "every declared matrix must converge");
        if (known)
        {
            const auto error = match(state, std::span{expected}.first(n));
            if (error > 1e-9) std::cerr << "root error: " << name << " n=" << n << " error=" << error << std::endl;
            require(error <= 1e-9, "analytic root accuracy");
            maximumError = std::max(maximumError, error);
            ++analytic;
        }
        ++cases;
        maximumWork = std::max(maximumWork, state.consumedWork());
        maximumIterations = std::max(maximumIterations, state.iterations());
        if (dump) dumpEigen(state, std::span{matrix}.first(n * n), name);
    };
    Matrix matrix{};
    Roots expected{};
    matrix[0] = expected[0] = {3, -4};
    run(matrix, 1, expected, true, "scalar");
    matrix = {}; expected = {};
    run(matrix, 16, expected, true, "zero");
    run(matrix, Eigen::maximumSize, expected, true, "zero_32");
    matrix = {}; expected = {};
    for (std::size_t i = 0; i < 16; ++i) matrix[i * 16 + i] = expected[i] = Complex{.2 * i, .3 - .1 * i};
    run(matrix, 16, expected, true, "diagonal");
    matrix = {}; expected = {};
    for (std::size_t i = 0; i < Eigen::maximumSize; ++i)
        matrix[i * Eigen::maximumSize + i] = expected[i] = Complex{.2 * i, .3 - .1 * i};
    run(matrix, Eigen::maximumSize, expected, true, "diagonal_32");
    for (std::size_t n = 2; n <= Eigen::maximumSize; n += 2)
    {
        matrix = {};
        auto q = [n](std::size_t row, std::size_t column)
        { return std::sqrt((column == 0 ? 1.0 : 2.0) / n) * std::cos(std::numbers::pi * (row + .5) * column / n); };
        for (std::size_t k = 0; k < n; k += 2)
        {
            const auto angle = .17 + .19 * k;
            expected[k] = std::polar(1.0, angle); expected[k + 1] = std::conj(expected[k]);
            for (std::size_t i = 0; i < n; ++i)
                for (std::size_t j = 0; j < n; ++j)
                    matrix[i * n + j] += (1.0 + i / 3.0) / (1.0 + j / 3.0)
                        * (std::cos(angle) * (q(i,k) * q(j,k) + q(i,k+1) * q(j,k+1))
                           + std::sin(angle) * (-q(i,k) * q(j,k+1) + q(i,k+1) * q(j,k)));
        }
        run(matrix, n, expected, true, "mixed_rotations");
    }
    for (std::size_t n = 1; n <= Eigen::maximumSize; ++n)
    { matrix = complexSimilarity(n, expected); run(matrix, n, expected, true, "complex_similarity"); }
    matrix = {};
    for (std::size_t k = 0; k < Eigen::maximumSize; ++k)
        expected[k] = std::polar(.9, .13 + .025 * (k / 4) + .00005 * (k % 4));
    for (std::size_t i = 0; i < Eigen::maximumSize; ++i)
        for (std::size_t j = 0; j < Eigen::maximumSize; ++j)
        {
            for (std::size_t k = 0; k < Eigen::maximumSize; ++k)
                matrix[i * Eigen::maximumSize + j] += expected[k] * std::polar(1.0 / Eigen::maximumSize,
                    tau * (static_cast<double>(i) - j) * k / Eigen::maximumSize);
            matrix[i * Eigen::maximumSize + j] *= (1.0 + i / 16.0) / (1.0 + j / 16.0);
        }
    run(matrix, Eigen::maximumSize, expected, true, "clustered_rotated_32");
    matrix = {};
    for (std::size_t i = 0; i < 16; ++i)
    {
        matrix[i * 16 + i] = expected[i] = {1, .5};
        if (i + 1 < 16) matrix[i * 16 + i + 1] = {1, -.2};
    }
    run(matrix, 16, expected, true, "triangular_jordan");
    matrix = {}; expected = {};
    for (std::size_t i = 0; i < Eigen::maximumSize; ++i)
    {
        matrix[i * Eigen::maximumSize + i] = expected[i] = {1, .5};
        if (i + 1 < Eigen::maximumSize) matrix[i * Eigen::maximumSize + i + 1] = {1, -.2};
    }
    run(matrix, Eigen::maximumSize, expected, true, "triangular_jordan_32");
    matrix = {};
    expected = {};
    for (std::size_t i = 0; i < 16; ++i)
    {
        matrix[i * 16 + (i + 1) % 16] = 1;
        expected[i] = std::polar(1.0, tau * i / 16);
    }
    run(matrix, 16, expected, true, "cycle");
    matrix = {}; expected = {};
    for (std::size_t i = 0; i < Eigen::maximumSize; ++i)
    {
        matrix[i * Eigen::maximumSize + (i + 1) % Eigen::maximumSize] = 1;
        expected[i] = std::polar(1.0, tau * i / Eigen::maximumSize);
    }
    run(matrix, Eigen::maximumSize, expected, true, "cycle_32");
    matrix = {};
    const auto coefficient = 1 + 2 * std::cos(.7);
    matrix[2] = 1; matrix[3] = 1; matrix[5] = -coefficient; matrix[7] = 1; matrix[8] = coefficient;
    expected[0] = 1; expected[1] = std::polar(1.0, .7); expected[2] = std::conj(expected[1]);
    run(matrix, 3, expected, true, "companion");
    matrix = {};
    const auto c1 = 2 * std::cos(.7), c2 = 2 * std::cos(.7001);
    matrix[3] = -1; matrix[4] = 1; matrix[7] = c1 + c2;
    matrix[9] = 1; matrix[11] = -2 - c1 * c2; matrix[14] = 1; matrix[15] = c1 + c2;
    expected[0] = std::polar(1.0, .7); expected[1] = std::conj(expected[0]);
    expected[2] = std::polar(1.0, .7001); expected[3] = std::conj(expected[2]);
    run(matrix, 4, expected, true, "clustered_companion");
    for (const auto mode : {0, 1, 2})
    {
        matrix = {};
        const auto center = mode == 2 ? Complex{0, 1} : Complex{1, 0};
        const auto coupling = mode == 0 ? 1e-13 : 1e-16;
        matrix[0] = matrix[3] = center; matrix[1] = 1; matrix[2] = coupling;
        expected[0] = center + std::sqrt(coupling); expected[1] = center - std::sqrt(coupling);
        run(matrix, 2, expected, true, "sensitive_coupling");
    }
    for (const auto scale : {1e-200, 1e200})
    {
        matrix = complexSimilarity(4, expected);
        for (auto& value : matrix) value *= scale;
        for (auto& value : expected) value *= scale;
        // Score in normalized units, including the extremely small case.
        Eigen normalized;
        require(normalized.begin(std::span{matrix}.first(16), 4), "scaled admission");
        finish(normalized);
        run(matrix, 4, expected, true, "scaled_similarity");
        Roots unscaled{}; complexSimilarity(4, unscaled);
        std::array<bool, 4> used{};
        for (std::size_t i = 0; i < 4; ++i)
        {
            auto nearest = std::size_t{4}; double distance = 1;
            for (std::size_t j = 0; j < 4; ++j)
                if (!used[j] && std::abs(normalized.eigenvalue(j) / scale - unscaled[i]) < distance)
                { nearest = j; distance = std::abs(normalized.eigenvalue(j) / scale - unscaled[i]); }
            require(nearest < 4 && distance <= 1e-9, "uniform scaling preserves roots"); used[nearest] = true;
        }
    }
    std::uint32_t seed = 271828;
    auto random = [&] { seed = 1664525U * seed + 1013904223U; return static_cast<double>(seed) / 4294967296.0 - .5; };
    for (const auto complex : {false, true})
        for (std::size_t n = 1; n <= Eigen::maximumSize; ++n)
        {
            for (auto& value : matrix) { const auto real = random(); value = {real, complex ? random() : 0}; }
            run(matrix, n, {}, false, complex ? "random_complex" : "random_real");
        }
    constexpr auto expectedAnalytic = 17 + 3 * Eigen::maximumSize / 2;
    require(cases == expectedAnalytic + 2 * Eigen::maximumSize && analytic == expectedAnalytic,
            "complete declared eigen fixture inventory");
    if (!dump) std::cout << "eigen_cases=" << cases << " analytic_cases=" << analytic << " max_error=" << maximumError
        << " max_work=" << maximumWork << " max_iterations=" << maximumIterations << " max_whole_job_ms=" << maximumMs << std::endl;
}

void testEigenState()
{
    Eigen state;
    auto work = std::size_t{17}; state.advance(work);
    require(work == 17 && state.maximumWork() == 0, "idle eigen solver inert");
    const std::array invalid{Complex{0, std::numeric_limits<double>::quiet_NaN()}};
    require(!state.begin(invalid, 1) && !state.begin({}, 0)
            && !state.begin({}, Eigen::maximumSize + 1), "invalid input rejected");
    const std::array lostCoupling{Complex{1}, Complex{1e300}, Complex{1e-300}, Complex{1}};
    require(!state.begin(lostCoupling, 2) && state.status() == Eigen::Status::DynamicRangeLoss
            && state.eigenvalue(0) == Complex{}, "normalization must not silently lose a nonzero coupling");
    Roots expected{};
    auto matrix = complexSimilarity(Eigen::maximumSize, expected);
    require(!state.begin(std::span{matrix}.first(4), 3), "shape mismatch rejected");
    require(state.begin(matrix, Eigen::maximumSize), "state reference capture"); finish(state);
    for (const auto partition : {1U, 97U, 65536U})
    {
        Eigen split;
        require(split.begin(matrix, Eigen::maximumSize), "partition capture");
        auto zero = std::size_t{0}; split.advance(zero);
        require(split.consumedWork() == 0 && split.eigenvalue(0) == Complex{}, "no partial results or zero work");
        finish(split, partition); same(state, split);
    }
    std::size_t copies = 0;
    for (const auto position : {1U, 15U, 16U, 31U, 61U, 255U, 511U, 1023U, 4095U, 8191U, 20000U, 100000U})
    {
        Eigen source, destination = state;
        require(source.begin(matrix, Eigen::maximumSize), "pending eigen capture");
        auto budget = static_cast<std::size_t>(position); source.advance(budget);
        require(source.status() == Eigen::Status::Running, "pending checkpoint covers active QR/reduction");
        withoutAllocations([&] { destination = source; source.reset(); return true; });
        finish(destination, 701); same(state, destination); ++copies;
    }
    Eigen captured;
    require(captured.begin(matrix, Eigen::maximumSize), "owned eigen input");
    matrix.fill({std::numeric_limits<double>::quiet_NaN(), 0});
    finish(captured); same(state, captured);
    const auto large = std::numeric_limits<double>::max();
    const std::array overflow{Complex{large}, Complex{large}, Complex{large}, Complex{large}};
    require(captured.begin(overflow, 2), "finite extreme input admission"); finish(captured);
    require(captured.status() == Eigen::Status::NumericOverflow && captured.eigenvalue(0) == Complex{}, "overflow hides all roots");
    captured.reset();
    require(captured.status() == Eigen::Status::Idle && captured.maximumWork() == 0 && captured.eigenvalue(0) == Complex{}, "reset hides roots");
    std::cout << "eigen_checkpoint_positions=" << copies << " sizeof_eigen=" << sizeof(Eigen) << std::endl;
}

void dumpModes(const Subspace& subspace, const Modes& state, bool quantized, std::string_view name)
{
    std::cout << std::setprecision(17) << "{\"kind\":\"modes\",\"name\":\"" << name
        << "\",\"rows\":" << subspace.rows() << ",\"size\":" << state.size()
        << ",\"quantized\":" << (quantized ? "true" : "false") << ",\"basis\":[";
    for (std::size_t row = 0; row < subspace.rows(); ++row)
    {
        std::cout << (row ? ",[" : "[");
        for (std::size_t column = 0; column < state.size(); ++column)
            std::cout << (column ? "," : "") << subspace.basis(column, row);
        std::cout << ']';
    }
    std::cout << "],\"shift\":[";
    for (std::size_t row = 0; row < state.size(); ++row)
    {
        std::cout << (row ? ",[" : "[");
        for (std::size_t column = 0; column < state.size(); ++column)
            std::cout << (column ? "," : "") << state.shift(row, column);
        std::cout << ']';
    }
    std::cout << ']'; rootsJson(state); std::cout << "}" << std::endl;
}

void testModes(bool dump)
{
    std::size_t cases = 0, clean = 0, maximumWork = 0, maximumIterations = 0;
    double maximumError = 0, maximumMs = 0;
    talas::test::lag_subspace::forEachProfile([&](auto frequencies, auto amplitudes, bool quantized, double center, auto name)
    {
        const auto sequence = talas::test::lag_subspace::sourceDifferences(frequencies, amplitudes, quantized, center);
        Subspace subspace;
        require(subspace.begin(sequence, 95), "mode source capture"); finish(subspace);
        Modes state;
        require(withoutAllocations([&] { return state.begin(subspace, 2 * frequencies.size()); }), "mode basis capture");
        const auto start = std::chrono::steady_clock::now(); finish(state);
        maximumMs = std::max(maximumMs, std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
        require(state.status() == Modes::Status::Ready, "stationary row-shift modes converge");
        if (!quantized)
        {
            Roots expected{};
            for (std::size_t i = 0; i < frequencies.size(); ++i)
            { expected[2 * i] = std::polar(1.0, tau * frequencies[i] / 1500); expected[2 * i + 1] = std::conj(expected[2 * i]); }
            const auto error = match(state, std::span{expected}.first(state.size()));
            require(error <= 1e-9, "analytic clean source modes"); maximumError = std::max(maximumError, error); ++clean;
        }
        ++cases; maximumWork = std::max(maximumWork, state.consumedWork());
        maximumIterations = std::max(maximumIterations, state.eigenIterations());
        if (dump) dumpModes(subspace, state, quantized, name);
    });

    constexpr std::size_t partialCount = Eigen::maximumSize / 2;
    std::array<double, partialCount> frequencies{}, amplitudes{};
    for (std::size_t i = 0; i < partialCount; ++i)
    { frequencies[i] = 45.0 + 37.0 * i; amplitudes[i] = .1; }
    const auto sequence = maximumModeSequence();
    Subspace subspace;
    require(subspace.begin(sequence, 95), "maximum-mode source capture"); finish(subspace);
    require(subspace.rank() >= Eigen::maximumSize, "source retains 32 independent basis modes");
    Modes state;
    require(withoutAllocations([&] { return state.begin(subspace, Eigen::maximumSize); }), "maximum-mode basis capture");
    const auto start = std::chrono::steady_clock::now(); finish(state);
    maximumMs = std::max(maximumMs, std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count());
    require(state.status() == Modes::Status::Ready && state.size() == Eigen::maximumSize,
            "all 32 exponential modes converge");
    Roots expected{};
    for (std::size_t i = 0; i < partialCount; ++i)
    {
        expected[2 * i] = std::polar(1.0, tau * frequencies[i] / 1500);
        expected[2 * i + 1] = std::conj(expected[2 * i]);
    }
    const auto error = match(state, expected);
    require(error <= 1e-9, "analytic 32-mode source roots");
    maximumError = std::max(maximumError, error);
    maximumWork = std::max(maximumWork, state.consumedWork());
    maximumIterations = std::max(maximumIterations, state.eigenIterations());
    if (dump) dumpModes(subspace, state, false, "maximum_32_modes");
    ++cases;
    ++clean;

    Modes source, destination;
    require(source.begin(subspace, Eigen::maximumSize), "maximum-mode checkpoint capture");
    auto budget = std::size_t{97000};
    withoutAllocations([&] { source.advance(budget); return true; });
    require(source.status() == Modes::Status::Running, "maximum-mode checkpoint is pending");
    withoutAllocations([&] { destination = source; source.reset(); return true; });
    finish(destination, 701);
    same(state, destination);
    for (std::size_t i = 0; i < Eigen::maximumSize; ++i)
        for (std::size_t j = 0; j < Eigen::maximumSize; ++j)
            require(state.shift(i, j) == destination.shift(i, j), "maximum-mode checkpoint shift matrix");

    require(cases == 37 && clean == 23, "complete source inventory including 32-mode profile");
    if (!dump) std::cout << "mode_cases=" << cases << " clean_cases=" << clean << " max_clean_error=" << maximumError
        << " max_work=" << maximumWork << " max_iterations=" << maximumIterations << " max_whole_job_ms=" << maximumMs << std::endl;
}

void testModeState()
{
    Modes state;
    auto work = std::size_t{17}; state.advance(work);
    require(work == 17 && state.maximumWork() == 0, "idle modes inert");
    Subspace subspace;
    require(!state.begin(subspace, 2), "unfinished subspace rejected");
    const std::array frequencies{20.0, 28.0}, amplitudes{.25, .08};
    const auto sequence = talas::test::lag_subspace::sourceDifferences(frequencies, amplitudes);
    require(subspace.begin(sequence, 95), "mode checkpoint subspace"); finish(subspace);
    for (const auto invalid : {0U, 1U, 3U, 6U, 18U,
             static_cast<unsigned>(Modes::maximumModes + 2)})
        require(!state.begin(subspace, invalid), "invalid mode count");
    require(state.begin(subspace, 4), "mode checkpoint reference"); finish(state);
    for (const auto partition : {1U, 97U, 65536U})
    {
        Modes split;
        require(split.begin(subspace, 4), "mode partition capture");
        auto zero = std::size_t{0}; split.advance(zero);
        require(split.consumedWork() == 0 && split.shift(0,0) == 0 && split.eigenvalue(0) == Complex{}, "hidden pending modes");
        finish(split, partition); same(state, split);
    }
    std::size_t copies = 0;
    for (const auto position : {1U, 95U, 96U, 1535U, 1536U, 1540U, 1544U, 1548U, 1555U, 1560U, 1600U, 1700U, 2000U, 2500U})
    {
        Modes source, destination = state;
        require(source.begin(subspace, 4), "pending mode capture");
        auto budget = static_cast<std::size_t>(position); source.advance(budget);
        require(source.status() == Modes::Status::Running, "pending matrix/solve/QR checkpoint");
        withoutAllocations([&] { destination = source; source.reset(); return true; });
        finish(destination, 701); same(state, destination);
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t j = 0; j < 4; ++j) require(state.shift(i,j) == destination.shift(i,j), "exact checkpoint shift");
        ++copies;
    }
    Modes captured;
    require(captured.begin(subspace, 4), "owned basis capture"); subspace.reset();
    finish(captured); same(state, captured);
    require(captured.eigenvalue(4) == Complex{} && captured.shift(4,0) == 0, "out-of-range modes inaccessible");
    const std::array singular{0.0, 0.0, 1.0, 0.0};
    require(subspace.begin(singular, 2), "singular prefix fixture"); finish(subspace);
    require(captured.begin(subspace, 2), "rank-two basis with singular U0"); finish(captured);
    require(captured.status() == Modes::Status::SingularShift && captured.eigenvalue(0) == Complex{}
            && captured.shift(0,0) == 0, "rank-deficient prefix rejected without partial data");
    const std::array square{1.0, 2.0, 3.0};
    require(subspace.begin(square, 2), "square basis"); finish(subspace);
    require(!captured.begin(subspace, 2), "too few rows rejected");
    withoutAllocations([&] { captured.reset(); return true; });
    require(captured.status() == Modes::Status::Idle && captured.maximumWork() == 0, "mode reset invalidates all state");
    std::cout << "mode_checkpoint_positions=" << copies << " sizeof_modes=" << sizeof(Modes) << std::endl;
}

template <class State> void probeState(State state, std::size_t partition, std::string_view name)
{
    // Prepared storage and state copies are outside the timed advance calls.
    std::array<double, 80000> durations{};
    std::size_t calls = 0;
    while (state.status() == State::Status::Running)
    {
        require(calls < durations.size(), "timing storage covers the declared bound");
        auto work = partition;
        const auto start = std::chrono::steady_clock::now();
        state.advance(work);
        durations[calls++] = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        require(state.consumedWork() <= state.maximumWork(), "timed work within bound");
    }
    require(state.status() == State::Status::Ready, "timed job converges");
    std::sort(durations.begin(), durations.begin() + static_cast<std::ptrdiff_t>(calls));
    std::cout << "primitive_budget name=" << name << " budget=" << partition << " calls=" << calls
              << " work=" << state.consumedWork() << " p99_us=" << durations[static_cast<std::size_t>((calls - 1) * .99)]
              << " max_us=" << durations[calls - 1] << std::endl;
}

void probeBudgets()
{
    Roots expected{};
    const auto matrix = complexSimilarity(16, expected);
    Eigen eigen;
    require(eigen.begin(matrix, 16), "budget eigen capture");
    const std::array frequencies{91.1, 171.3, 251.9, 332.3, 411.7, 493.1, 573.7, 653.9};
    std::array<double, 8> amplitudes{};
    for (std::size_t i = 0; i < amplitudes.size(); ++i) amplitudes[i] = .25 / (i + 1);
    const auto sequence = talas::test::lag_subspace::sourceDifferences(frequencies, amplitudes);
    Subspace subspace;
    require(subspace.begin(sequence, 95), "budget basis capture"); finish(subspace);
    Modes modes;
    require(modes.begin(subspace, 16), "budget mode capture");
    for (const auto partition : {512U, 2048U, 8192U})
    { probeState(eigen, partition, "eigen16"); probeState(modes, partition, "modes16"); }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--budget-probe")
        { probeBudgets(); return 0; }
        const auto dump = argc == 2 && std::string_view{argv[1]} == "--dump";
        testEigenFixtures(dump);
        if (!dump) testEigenState();
        testModes(dump);
        if (!dump) { testModeState(); std::cout << "Incremental subspace modes tests passed" << std::endl; }
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << "Incremental subspace modes tests failed: " << error.what() << std::endl;
        return 1;
    }
}
