#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::dsp
{
// Bounded owned matrix with resumable Hessenberg reduction and complex QR.
// Used by stationary-component estimation; it does not identify source frequencies.
class IncrementalComplexEigenvalues
{
public:
    using Complex = std::complex<double>;
    static constexpr std::size_t maximumSize = 32, maximumIterations = 1024;
    static constexpr double deflationTolerance = std::numeric_limits<double>::epsilon();
    enum class Status { Idle, Running, Ready, InvalidInput, NumericOverflow, DynamicRangeLoss, DidNotConverge };

    // Copies a finite row-major square matrix now, at most 1024 entries.
    [[nodiscard]] bool begin(std::span<const Complex> matrix, std::size_t size) noexcept;
    // One unit is a scalar product/update or bounded stage decision. This bounds
    // arithmetic work, not elapsed time. Zero budget is inert; unused work returns.
    void advance(std::size_t& work) noexcept;
    void reset() noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t iterations() const noexcept { return iterations_; }
    [[nodiscard]] std::size_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] static constexpr std::size_t maximumWorkForSize(std::size_t n) noexcept
    {
        return n == 0 || n > maximumSize ? 0 : 8 * n * n * n + 16 * n * n + maximumIterations
            * (8 * n * n * n + 16 * n * n + 16 * n + 32) + 4 * n + 32;
    }
    [[nodiscard]] std::size_t maximumWork() const noexcept { return maximumWorkForSize(size_); }
    // Unordered roots; unavailable results return zero, including partial solves.
    [[nodiscard]] Complex eigenvalue(std::size_t index) const noexcept;

private:
    enum class Stage { Check, Shift, Setup, Norm, Vector, RDot, RApply, QDot, QApply, Product };
    void nextReflector() noexcept;
    void deflate() noexcept;
    [[nodiscard]] bool canDeflate() const noexcept;
    [[nodiscard]] static bool finite(Complex value) noexcept;
    static constexpr std::size_t offset(std::size_t row, std::size_t column) noexcept
    { return row * maximumSize + column; }

    std::array<Complex, maximumSize * maximumSize> matrix_{}, r_{}, q_{};
    std::array<Complex, maximumSize> reflector_{}, roots_{};
    Status status_ = Status::Idle;
    Stage stage_ = Stage::Check;
    std::size_t size_ = 0, active_ = 0, iterations_ = 0, sinceDeflation_ = 0, consumedWork_ = 0;
    std::size_t index_ = 0, column_ = 0, start_ = 0, target_ = 0, row_ = 0, inner_ = 0;
    bool reducing_ = false;
    double scale_ = 0, matrixNorm_ = 0, rowEnergy_ = 0, norm_ = 0, vectorEnergy_ = 0, beta_ = 0;
    Complex shift_{}, phase_{}, sum_{};
};
}
