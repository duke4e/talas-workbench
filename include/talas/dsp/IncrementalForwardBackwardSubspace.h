#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace talas::dsp
{
// Fixed-storage, resumable left basis for the even/odd split of [H,J_rows H].
// The input is the 191-sample difference sequence for the fixed 97x95 Hankel
// shape. This is a numerical primitive, not a source-frequency estimator.
class IncrementalForwardBackwardSubspace
{
public:
    static constexpr std::size_t differenceCount = 191;
    static constexpr std::size_t rowsCount = 97;
    static constexpr std::size_t hankelColumns = 95;
    static constexpr std::size_t evenColumns = 49;
    static constexpr std::size_t oddColumns = 48;
    static constexpr std::size_t maximumModes = evenColumns + oddColumns;
    static constexpr std::size_t maximumSweeps = 24;
    static constexpr double correlationTolerance = 1e-12;
    static constexpr double relativeSingularFloor = 1e-12;

    enum class Status { Idle, Running, Ready, InvalidInput, NumericOverflow, DidNotConverge };

    // Snapshot and normalize the exact fixed-size difference sequence. No
    // caller pointer is retained and begin performs no dynamic allocation.
    [[nodiscard]] bool begin(std::span<const double> differences) noexcept;
    // One unit is one matrix-energy/projection element, factor norm or V fill,
    // dot/data/V rotation element, final norm element, or ordering/rank step.
    // This bounds arithmetic work, not elapsed time. Zero work is inert.
    void advance(std::size_t& work) noexcept;
    void reset() noexcept;

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] constexpr std::size_t rows() const noexcept { return rowsCount; }
    [[nodiscard]] std::size_t rank() const noexcept { return status_ == Status::Ready ? rank_ : 0; }
    [[nodiscard]] std::size_t evenSweeps() const noexcept { return sweeps_[0]; }
    [[nodiscard]] std::size_t oddSweeps() const noexcept { return sweeps_[1]; }
    [[nodiscard]] std::size_t evenRotations() const noexcept { return rotations_[0]; }
    [[nodiscard]] std::size_t oddRotations() const noexcept { return rotations_[1]; }
    [[nodiscard]] std::size_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] std::size_t maximumWork() const noexcept;
    [[nodiscard]] static constexpr std::size_t maximumWorkForShape() noexcept
    {
        constexpr auto factorWork = [](std::size_t columns)
        {
            return 2 * hankelColumns * columns
                + maximumSweeps * columns * (columns - 1) / 2
                    * (2 * hankelColumns + 1 + columns)
                + columns * columns;
        };
        // A quadratic allowance covers all comparisons, swaps, and rank tests.
        constexpr auto orderAndRankMaximum = maximumModes * maximumModes + 2 * maximumModes;
        return 3 * rowsCount * hankelColumns + factorWork(evenColumns) + factorWork(oddColumns)
            + orderAndRankMaximum;
    }
    // Descending physical singular values; the mapped basis columns are
    // normalized. Out-of-range or unretained entries return zero.
    [[nodiscard]] double singularValue(std::size_t mode) const noexcept;
    [[nodiscard]] double basis(std::size_t mode, std::size_t row) const noexcept;

private:
    friend struct IncrementalForwardBackwardSubspaceTestAccess;
    enum class Stage
    {
        Energy, Projection, FactorNorm, FactorRotationInit, Dot, Decide,
        RotateData, RotateVectors, FinalNorm, Sort, Rank
    };

    [[nodiscard]] std::size_t factorColumns() const noexcept;
    [[nodiscard]] double& factorValue(std::size_t factor, std::size_t column, std::size_t row) noexcept;
    [[nodiscard]] double factorValue(std::size_t factor, std::size_t column, std::size_t row) const noexcept;
    [[nodiscard]] double& factorNorm(std::size_t factor, std::size_t column) noexcept;
    [[nodiscard]] double factorNorm(std::size_t factor, std::size_t column) const noexcept;
    [[nodiscard]] double& factorRotation(std::size_t factor, std::size_t row, std::size_t column) noexcept;
    [[nodiscard]] double factorRotation(std::size_t factor, std::size_t row, std::size_t column) const noexcept;
    void nextPair() noexcept;
    void finishFactorNorms() noexcept;

    std::array<double, differenceCount> sequence_{};
    std::array<double, evenColumns * hankelColumns> evenValues_{};
    std::array<double, oddColumns * hankelColumns> oddValues_{};
    std::array<double, evenColumns * evenColumns> evenRotations_{};
    std::array<double, oddColumns * oddColumns> oddRotations_{};
    std::array<double, evenColumns> evenNorms_{};
    std::array<double, oddColumns> oddNorms_{};
    std::array<double, maximumModes> singularValues_{};
    std::array<std::size_t, maximumModes> order_{};

    Status status_ = Status::Idle;
    Stage stage_ = Stage::Energy;
    std::size_t rank_ = 0, consumedWork_ = 0;
    std::size_t energyIndex_ = 0, projectionIndex_ = 0;
    std::size_t factor_ = 0, column_ = 0, row_ = 0, rotationIndex_ = 0;
    std::size_t first_ = 0, second_ = 1, index_ = 0;
    std::array<std::size_t, 2> sweeps_{};
    std::array<std::size_t, 2> rotations_{};
    std::size_t rotationsInSweep_ = 0, sortIndex_ = 1, sortPosition_ = 1, rankIndex_ = 0;
    double scale_ = 0.0, energy_ = 0.0, alpha_ = 0.0, beta_ = 0.0, cross_ = 0.0;
    double cosine_ = 1.0, sine_ = 0.0;
};
}
