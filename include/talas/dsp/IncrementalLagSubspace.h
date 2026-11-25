#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace talas::dsp
{
// Fixed-storage research primitive for stationary-component estimation. This is
// not yet part of PreparedTimeFrequencyScale's callback or latency contract.
// Both axes of a Hankel lag matrix contain the same exponential modes; rotating
// its columns gives a temporal basis without accumulating a second matrix.
class IncrementalLagSubspace
{
public:
    static constexpr std::size_t maximumSequenceSize = 191;
    static constexpr std::size_t maximumColumns = 95;
    static constexpr std::size_t maximumValues = 97 * maximumColumns;
    static constexpr std::size_t maximumSweeps = 24;
    static constexpr double correlationTolerance = 1e-12;
    static constexpr double relativeSingularFloor = 1e-12;

    enum class Status { Idle, Running, Ready, InvalidInput, NumericOverflow, DidNotConverge };

    // Snapshot the bounded sequence now; no source pointer is retained. The
    // matrix is formed incrementally, with rows=sequence.size()-columns+1.
    [[nodiscard]] bool begin(std::span<const double> sequence, std::size_t columns) noexcept;
    // One unit is a matrix fill/norm row, pair dot/rotation row, pair decision,
    // or insertion-sort step. This is a work bound, not a CPU-time guarantee.
    // Zero budget changes no state. Unused work is returned to the caller.
    void advance(std::size_t& work) noexcept;
    void reset() noexcept;

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] std::size_t rows() const noexcept { return rows_; }
    [[nodiscard]] std::size_t columns() const noexcept { return columns_; }
    [[nodiscard]] std::size_t rank() const noexcept { return status_ == Status::Ready ? rank_ : 0; }
    [[nodiscard]] std::size_t sweeps() const noexcept { return sweeps_; }
    [[nodiscard]] std::size_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] std::size_t maximumWork() const noexcept;
    // Descending singular values; normalized basis columns. Unavailable or
    // rank-deficient entries return zero. Signs inside a subspace are arbitrary.
    [[nodiscard]] double singularValue(std::size_t mode) const noexcept;
    [[nodiscard]] double basis(std::size_t mode, std::size_t row) const noexcept;

private:
    enum class Stage { Fill, Dot, Decide, Rotate, Norm, Sort, Rank };
    void nextPair() noexcept;
    void beginNorms() noexcept;

    std::array<double, maximumSequenceSize> sequence_{};
    std::array<double, maximumValues> values_{};
    std::array<double, maximumColumns> norms_{};
    std::array<std::size_t, maximumColumns> order_{};
    Status status_ = Status::Idle;
    Stage stage_ = Stage::Fill;
    std::size_t rows_ = 0, columns_ = 0, rank_ = 0;
    std::size_t index_ = 0, first_ = 0, second_ = 1;
    std::size_t sweeps_ = 0, rotations_ = 0, consumedWork_ = 0;
    std::size_t sortIndex_ = 1, sortPosition_ = 1;
    double scale_ = 0, energy_ = 0, alpha_ = 0, beta_ = 0, cross_ = 0;
    double cosine_ = 1, sine_ = 0;
};
}
