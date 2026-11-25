#include "talas/dsp/IncrementalForwardBackwardSubspace.h"
#include "../support/LagSubspaceStationaryReference.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
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
struct IncrementalForwardBackwardSubspaceTestAccess
{
    using Subspace = IncrementalForwardBackwardSubspace;
    static int stage(const Subspace& value) { return static_cast<int>(value.stage_); }
    static std::size_t factor(const Subspace& value) { return value.factor_; }
    static std::size_t energyIndex(const Subspace& value) { return value.energyIndex_; }
    static std::size_t projectionIndex(const Subspace& value) { return value.projectionIndex_; }
    static std::size_t column(const Subspace& value) { return value.column_; }
    static std::size_t row(const Subspace& value) { return value.row_; }
    static std::size_t index(const Subspace& value) { return value.index_; }
    static std::size_t rotationIndex(const Subspace& value) { return value.rotationIndex_; }
    static std::size_t sortIndex(const Subspace& value) { return value.sortIndex_; }
    static std::size_t sortPosition(const Subspace& value) { return value.sortPosition_; }
    static std::size_t rankIndex(const Subspace& value) { return value.rankIndex_; }
    static constexpr int energy = static_cast<int>(Subspace::Stage::Energy);
    static constexpr int projection = static_cast<int>(Subspace::Stage::Projection);
    static constexpr int factorNorm = static_cast<int>(Subspace::Stage::FactorNorm);
    static constexpr int factorRotationInit = static_cast<int>(Subspace::Stage::FactorRotationInit);
    static constexpr int dot = static_cast<int>(Subspace::Stage::Dot);
    static constexpr int decide = static_cast<int>(Subspace::Stage::Decide);
    static constexpr int rotateData = static_cast<int>(Subspace::Stage::RotateData);
    static constexpr int rotateVectors = static_cast<int>(Subspace::Stage::RotateVectors);
    static constexpr int finalNorm = static_cast<int>(Subspace::Stage::FinalNorm);
    static constexpr int sort = static_cast<int>(Subspace::Stage::Sort);
    static constexpr int rank = static_cast<int>(Subspace::Stage::Rank);
};
}

namespace
{
using Subspace = talas::dsp::IncrementalForwardBackwardSubspace;
using Status = Subspace::Status;
using Access = talas::dsp::IncrementalForwardBackwardSubspaceTestAccess;
constexpr auto tau = 2.0 * std::numbers::pi;
static_assert(std::is_trivially_copyable_v<Subspace>);
static_assert(Subspace::maximumWorkForShape() > 0);

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

const char* statusName(Status status)
{
    switch (status)
    {
    case Status::Idle: return "idle";
    case Status::Running: return "running";
    case Status::Ready: return "ready";
    case Status::InvalidInput: return "invalid_input";
    case Status::NumericOverflow: return "numeric_overflow";
    case Status::DidNotConverge: return "did_not_converge";
    }
    return "unknown";
}

void finish(Subspace& state, std::size_t partition = 4096)
{
    const auto bound = state.maximumWork();
    auto previous = state.consumedWork();
    while (state.status() == Status::Running)
    {
        auto budget = partition;
        allocations = 0;
        countAllocations = true;
        state.advance(budget);
        countAllocations = false;
        require(allocations == 0, "forward/backward advance must not allocate");
        require(budget <= partition && state.consumedWork() - previous == partition - budget,
                "advance must account for every unit consumed");
        require(state.consumedWork() > previous && state.consumedWork() <= bound,
                "pending work must advance within its declared bound");
        previous = state.consumedWork();
    }
}

void same(const Subspace& first, const Subspace& second)
{
    require(first.status() == second.status() && first.rank() == second.rank()
            && first.rows() == second.rows() && first.consumedWork() == second.consumedWork()
            && first.evenSweeps() == second.evenSweeps() && first.oddSweeps() == second.oddSweeps()
            && first.evenRotations() == second.evenRotations()
            && first.oddRotations() == second.oddRotations(),
            "partition and checkpoint metadata must match");
    for (std::size_t mode = 0; mode < first.rank(); ++mode)
    {
        require(first.singularValue(mode) == second.singularValue(mode), "checkpoint singular values");
        for (std::size_t row = 0; row < first.rows(); ++row)
            require(first.basis(mode, row) == second.basis(mode, row), "checkpoint mapped basis");
    }
}

std::array<double, Subspace::differenceCount> mixture(std::size_t sinusoidCount)
{
    std::array<double, 16> frequencies{}, amplitudes{};
    for (std::size_t i = 0; i < frequencies.size(); ++i)
    {
        frequencies[i] = 22.3 + 42.7 * static_cast<double>(i);
        amplitudes[i] = .2 / static_cast<double>(i + 1);
    }
    return talas::test::lag_subspace::sourceDifferences(
        std::span{frequencies}.first(sinusoidCount), std::span{amplitudes}.first(sinusoidCount));
}

std::array<double, Subspace::differenceCount> deterministicNoise()
{
    std::array<double, Subspace::differenceCount> sequence{};
    std::uint32_t seed = 271828;
    for (auto& value : sequence)
    {
        seed = 1664525U * seed + 1013904223U;
        value = static_cast<double>(seed) / 4294967296.0 - .5;
    }
    return sequence;
}

void complete(Subspace& state, std::span<const double> sequence, std::size_t partition = 4096)
{
    allocations = 0;
    countAllocations = true;
    const auto began = state.begin(sequence);
    countAllocations = false;
    require(began && allocations == 0, "fixed-size input capture must not allocate");
    finish(state, partition);
}

void verifySpectrumAndBasis(const Subspace& state, std::span<const double> sequence)
{
    require(state.status() == Status::Ready, "forward/backward decomposition converges");
    double expectedEnergy = 0.0;
    for (std::size_t column = 0; column < Subspace::hankelColumns; ++column)
        for (std::size_t row = 0; row < Subspace::rowsCount; ++row)
        {
            const auto direct = sequence[row + column];
            const auto reversed = sequence[Subspace::rowsCount - 1 - row + column];
            expectedEnergy += direct * direct + reversed * reversed;
        }
    double actualEnergy = 0.0;
    for (std::size_t mode = 0; mode < state.rank(); ++mode)
        actualEnergy += state.singularValue(mode) * state.singularValue(mode);
    require(std::abs(actualEnergy / expectedEnergy - 1.0) < 1e-9,
            "retained modes preserve the finite Hankel energy");
    for (std::size_t first = 0; first < state.rank(); ++first)
        for (std::size_t second = 0; second < state.rank(); ++second)
        {
            double dot = 0.0;
            for (std::size_t row = 0; row < state.rows(); ++row)
                dot += state.basis(first, row) * state.basis(second, row);
            require(std::abs(dot - (first == second ? 1.0 : 0.0)) < 1e-9,
                    "mapped singular basis is orthonormal");
        }
}

void dumpJson(const Subspace& state, std::span<const double> sequence, std::string_view name)
{
    std::cout << std::setprecision(17) << "{\"name\":\"" << name << "\",\"sequence\":[";
    for (std::size_t i = 0; i < sequence.size(); ++i)
        std::cout << (i == 0 ? "" : ",") << sequence[i];
    std::cout << "],\"status\":\"" << statusName(state.status()) << "\",\"rank\":" << state.rank()
              << ",\"singular_values\":[";
    for (std::size_t mode = 0; mode < state.rank(); ++mode)
        std::cout << (mode == 0 ? "" : ",") << state.singularValue(mode);
    std::cout << "],\"basis\":[";
    bool first = true;
    for (std::size_t row = 0; row < state.rows(); ++row)
        for (std::size_t mode = 0; mode < state.rank(); ++mode)
        {
            std::cout << (first ? "" : ",") << state.basis(mode, row);
            first = false;
        }
    std::cout << "],\"consumed_work\":" << state.consumedWork()
              << ",\"maximum_work\":" << state.maximumWork() << "}" << std::endl;
}

void emitFixture(std::span<const double> sequence, std::string_view name)
{
    Subspace state;
    complete(state, sequence);
    dumpJson(state, sequence, name);
}

void testInputAndSmallRanks()
{
    Subspace state;
    auto idleBudget = std::size_t{5};
    state.advance(idleBudget);
    require(idleBudget == 5 && state.status() == Status::Idle, "idle advance is inert");
    std::array<double, Subspace::differenceCount - 1> shortInput{};
    require(!state.begin(shortInput) && state.status() == Status::InvalidInput,
            "fixed input length is enforced");
    auto invalid = std::array<double, Subspace::differenceCount>{};
    invalid[17] = std::numeric_limits<double>::infinity();
    require(!state.begin(invalid) && state.status() == Status::InvalidInput,
            "nonfinite samples are rejected");
    std::array<double, Subspace::differenceCount> unrepresentableRatio{};
    unrepresentableRatio[0] = std::numeric_limits<double>::max();
    unrepresentableRatio[1] = std::numeric_limits<double>::denorm_min();
    require(!state.begin(unrepresentableRatio) && state.status() == Status::NumericOverflow,
            "normalization does not silently erase nonzero input");

    const std::array<double, Subspace::differenceCount> zero{};
    require(state.begin(zero) && state.status() == Status::Ready && state.rank() == 0
            && state.singularValue(0) == 0 && state.basis(0, 0) == 0,
            "zero input has no retained modes");
    const std::array<double, Subspace::differenceCount> constant = []
    {
        std::array<double, Subspace::differenceCount> values{};
        values.fill(1.0);
        return values;
    }();
    complete(state, constant);
    require(state.rank() == 1, "constant Hankel input has rank one");
    verifySpectrumAndBasis(state, constant);

    std::array<double, Subspace::differenceCount> overflowing{};
    overflowing.fill(std::numeric_limits<double>::max());
    require(state.begin(overflowing), "large but finite input can be normalized");
    finish(state);
    require(state.status() == Status::NumericOverflow && state.rank() == 0
            && state.singularValue(0) == 0 && state.basis(0, 0) == 0,
            "unrepresentable physical singular values expose no partial result");

    auto owned = mixture(2);
    Subspace expected, captured;
    complete(expected, owned);
    require(captured.begin(owned), "capture source-owned data");
    owned.fill(std::numeric_limits<double>::quiet_NaN());
    finish(captured);
    same(expected, captured);
    allocations = 0;
    countAllocations = true;
    captured.reset();
    countAllocations = false;
    require(allocations == 0 && captured.status() == Status::Idle && captured.rank() == 0
            && captured.maximumWork() == 0,
            "reset invalidates results without allocating");

    for (const auto& [count, expectedRank] : {std::pair{1U, 2U}, std::pair{2U, 4U},
                                                std::pair{6U, 12U}, std::pair{16U, 32U}})
    {
        const auto sequence = mixture(count);
        complete(state, sequence);
        require(state.rank() == expectedRank, "exact sinusoid order survives the split");
        verifySpectrumAndBasis(state, sequence);
    }
}

std::size_t workToNextStage(const Subspace& state)
{
    switch (Access::stage(state))
    {
    case Access::energy:
        return 2 * Subspace::rowsCount * Subspace::hankelColumns - Access::energyIndex(state);
    case Access::projection:
        return Subspace::rowsCount * Subspace::hankelColumns - Access::projectionIndex(state);
    case Access::factorNorm:
    {
        const auto columns = Access::factor(state) == 0 ? Subspace::evenColumns : Subspace::oddColumns;
        return (columns - Access::column(state)) * Subspace::hankelColumns - Access::row(state);
    }
    case Access::factorRotationInit:
    {
        const auto columns = Access::factor(state) == 0 ? Subspace::evenColumns : Subspace::oddColumns;
        return columns * columns - Access::rotationIndex(state);
    }
    case Access::dot:
        return Access::index(state) == 0 ? 1 : Subspace::hankelColumns - Access::index(state);
    case Access::decide:
        return 1;
    case Access::rotateData:
        return Subspace::hankelColumns - Access::index(state);
    case Access::rotateVectors:
    {
        const auto columns = Access::factor(state) == 0 ? Subspace::evenColumns : Subspace::oddColumns;
        return columns - Access::index(state);
    }
    case Access::finalNorm:
    {
        const auto columns = Access::factor(state) == 0 ? Subspace::evenColumns : Subspace::oddColumns;
        return (columns - Access::column(state)) * Subspace::hankelColumns - Access::row(state);
    }
    case Access::sort:
        return 1;
    case Access::rank:
        return Subspace::maximumModes - Access::rankIndex(state);
    default:
        return 1;
    }
}

void roundTripCopy(Subspace& state)
{
    Subspace checkpoint;
    allocations = 0;
    countAllocations = true;
    checkpoint = state;
    state = checkpoint;
    countAllocations = false;
    require(allocations == 0, "whole-state checkpoint copy allocates nothing");
}

void testPartitionsAndEveryStageCopy()
{
    const auto sequence = deterministicNoise();
    Subspace reference;
    complete(reference, sequence, 65536);
    require(reference.status() == Status::Ready && reference.rank() == Subspace::maximumModes,
            "finite deterministic noise spans the full left dimension");

    for (const auto partition : {std::size_t{113}, std::size_t{997}, std::size_t{65536}})
    {
        Subspace current;
        complete(current, sequence, partition);
        same(reference, current);
    }

    Subspace copied;
    require(copied.begin(sequence), "stage-copy input capture");
    const auto initialWork = copied.consumedWork();
    auto zeroBudget = std::size_t{0};
    copied.advance(zeroBudget);
    require(copied.consumedWork() == initialWork && zeroBudget == 0,
            "zero budget does not advance the factorization");
    std::array<std::array<bool, 11>, 2> seen{};
    while (copied.status() == Status::Running)
    {
        const auto factor = Access::factor(copied);
        const auto stage = Access::stage(copied);
        if (factor < seen.size() && stage >= 0 && static_cast<std::size_t>(stage) < seen[factor].size()
            && !seen[factor][static_cast<std::size_t>(stage)])
        {
            seen[factor][static_cast<std::size_t>(stage)] = true;
            roundTripCopy(copied);
        }
        auto work = workToNextStage(copied);
        require(work != 0, "stage boundary has positive remaining work");
        const auto before = copied.consumedWork();
        allocations = 0;
        countAllocations = true;
        copied.advance(work);
        countAllocations = false;
        require(allocations == 0, "stage-bounded advances allocate nothing");
        require(copied.consumedWork() > before && copied.consumedWork() <= copied.maximumWork(),
                "stage-bounded work remains inside the declared cap");
    }
    require(copied.status() == Status::Ready, "full-rank noise converges after copied checkpoints");
    same(reference, copied);
    // Energy/Projection run once before the even factor and Sort/Rank once
    // after the odd factor; every Jacobi substage runs for both factors.
    for (const auto factor : {std::size_t{0}, std::size_t{1}})
    {
        const auto first = factor == 0 ? std::size_t{0} : static_cast<std::size_t>(Access::factorNorm);
        const auto last = factor == 0 ? static_cast<std::size_t>(Access::finalNorm) : seen[factor].size() - 1;
        for (auto stage = first; stage <= last; ++stage)
            require(seen[factor][stage], "dirty-state copies cover each Jacobi substage and factor");
    }
}

void testWeakModeAndScaling()
{
    constexpr double amplitude = 4e-12;
    std::array<double, Subspace::differenceCount> weak{};
    for (std::size_t i = 0; i < weak.size(); ++i)
        weak[i] = 1.0 + amplitude * std::sin(tau * 7.0 * static_cast<double>(i)
                                              / static_cast<double>(weak.size()));
    Subspace state;
    complete(state, weak, 997);
    require(state.status() == Status::Ready && state.rank() == 3,
            "both distributed real modes are retained above the Frobenius rank floor");
    for (std::size_t i = 0; i < weak.size(); ++i)
        weak[i] = 1.0 + amplitude * .1 * std::sin(tau * 7.0 * static_cast<double>(i)
                                                   / static_cast<double>(weak.size()));
    complete(state, weak, 997);
    require(state.status() == Status::Ready && state.rank() == 1,
            "distributed weak mode below the rank floor is rejected");

    const auto source = mixture(4);
    Subspace reference;
    complete(reference, source);
    for (const auto scale : {1e-200, 1e200})
    {
        auto scaled = source;
        for (auto& value : scaled) value *= scale;
        complete(state, scaled, 997);
        require(state.status() == Status::Ready && state.rank() == reference.rank(),
                "normalization keeps rank stable at extreme finite scales");
        for (std::size_t mode = 0; mode < reference.rank(); ++mode)
        {
            require(std::abs(state.singularValue(mode) / scale / reference.singularValue(mode) - 1.0) < 1e-10,
                    "singular values return in physical input units");
            for (std::size_t row = 0; row < state.rows(); ++row)
                require(std::abs(state.basis(mode, row) - reference.basis(mode, row)) < 1e-12,
                        "basis is independent of overall input scale");
        }
    }
}

void runDump()
{
    std::array<double, Subspace::differenceCount> zero{};
    emitFixture(zero, "zero");
    auto constant = zero;
    constant.fill(1.0);
    emitFixture(constant, "constant");
    for (const auto& [count, label] : {std::pair{1U, std::string_view{"rank2"}},
                                       std::pair{2U, std::string_view{"rank4"}},
                                       std::pair{6U, std::string_view{"rank12"}},
                                       std::pair{16U, std::string_view{"rank32"}}})
    {
        const auto sequence = mixture(count);
        emitFixture(sequence, label);
    }
    const auto noise = deterministicNoise();
    emitFixture(noise, "noise97");
    constexpr double amplitude = 4e-12;
    std::array<double, Subspace::differenceCount> weak{};
    for (std::size_t i = 0; i < weak.size(); ++i)
        weak[i] = 1.0 + amplitude * std::sin(tau * 7.0 * static_cast<double>(i)
                                              / static_cast<double>(weak.size()));
    emitFixture(weak, "weak_mode");
    for (const auto scale : {1e-200, 1e200})
    {
        auto scaled = mixture(4);
        for (auto& value : scaled) value *= scale;
        emitFixture(scaled, scale < 1 ? "scaled_small" : "scaled_large");
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view{argv[1]} == "--dump-json")
        {
            runDump();
            return 0;
        }
        testInputAndSmallRanks();
        testPartitionsAndEveryStageCopy();
        testWeakModeAndScaling();
        std::cout << "Incremental forward/backward subspace tests passed; sizeof_state="
                  << sizeof(Subspace) << " maximum_work=" << Subspace::maximumWorkForShape() << std::endl;
        return 0;
    }
    catch (const std::exception& error)
    {
        countAllocations = false;
        std::cerr << "Incremental forward/backward subspace tests failed: " << error.what() << std::endl;
        return 1;
    }
}
