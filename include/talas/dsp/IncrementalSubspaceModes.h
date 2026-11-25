#pragma once

#include "talas/dsp/IncrementalComplexEigenvalues.h"
#include "talas/dsp/IncrementalLagSubspace.h"

namespace talas::dsp
{
class IncrementalForwardBackwardSubspace;

// Owned, resumable one-row shift regression on a real temporal basis. This
// extracts aliased exponential modes, not original-rate frequencies or a
// validated stationary source model.
class IncrementalSubspaceModes
{
public:
    using Complex = IncrementalComplexEigenvalues::Complex;
    static constexpr std::size_t maximumModes = IncrementalComplexEigenvalues::maximumSize;
    enum class Status { Idle, Running, Ready, InvalidInput, SingularShift, NumericOverflow, DidNotConverge };

    // Copies at most 191*32 basis entries now. No source reference is retained.
    [[nodiscard]] bool begin(const IncrementalLagSubspace& subspace, std::size_t modes) noexcept;
    [[nodiscard]] bool begin(const IncrementalForwardBackwardSubspace& subspace, std::size_t modes) noexcept;
    // One unit is a pair of dot-product terms, a matrix-cell solve update, a
    // bounded decision, or an eigenvalue work unit. Starting the eigenvalue
    // solver also snapshots at most 1024 entries in one bounded setup step.
    // This is an arithmetic bound, not a CPU-time guarantee.
    void advance(std::size_t& work) noexcept;
    void reset() noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] std::size_t size() const noexcept { return modes_; }
    [[nodiscard]] std::size_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] std::size_t eigenIterations() const noexcept { return eigenvalues_.iterations(); }
    [[nodiscard]] std::size_t maximumWork() const noexcept;
    [[nodiscard]] Complex eigenvalue(std::size_t index) const noexcept;
    // U0 * shift ~= U1, where U0/U1 omit the last/first basis row respectively.
    [[nodiscard]] double shift(std::size_t row, std::size_t column) const noexcept;

private:
    template <class Subspace>
    [[nodiscard]] bool beginSubspace(const Subspace& subspace, std::size_t modes) noexcept;
    enum class Stage { Accumulate, FindPivot, Swap, Normalize, SelectRow, Eliminate, NextPivot, Pack, Eigen };
    static constexpr std::size_t offset(std::size_t row, std::size_t column) noexcept
    { return row * maximumModes + column; }

    std::array<double, IncrementalLagSubspace::maximumSequenceSize * maximumModes> basis_{};
    std::array<double, maximumModes * maximumModes> gram_{}, shift_{};
    std::array<Complex, maximumModes * maximumModes> packed_{};
    IncrementalComplexEigenvalues eigenvalues_;
    Status status_ = Status::Idle;
    Stage stage_ = Stage::Accumulate;
    std::size_t rows_ = 0, modes_ = 0, consumedWork_ = 0;
    std::size_t row_ = 0, column_ = 0, index_ = 0, pivot_ = 0, selected_ = 0;
    double gramScale_ = 0, largest_ = 0, divisor_ = 0, factor_ = 0;
};
}
