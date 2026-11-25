#include "talas/dsp/IncrementalComplexEigenvalues.h"

#include <algorithm>
#include <cmath>

namespace talas::dsp
{
bool IncrementalComplexEigenvalues::finite(Complex value) noexcept
{ return std::isfinite(value.real()) && std::isfinite(value.imag()); }

bool IncrementalComplexEigenvalues::begin(std::span<const Complex> matrix, std::size_t size) noexcept
{
    reset();
    if (size == 0 || size > maximumSize || matrix.size() != size * size)
    { status_ = Status::InvalidInput; return false; }
    for (const auto value : matrix)
    {
        if (!finite(value)) { status_ = Status::InvalidInput; return false; }
        scale_ = std::max({scale_, std::abs(value.real()), std::abs(value.imag())});
    }
    size_ = active_ = size;
    for (std::size_t row = 0; row < size; ++row)
        for (std::size_t column = 0; column < size; ++column)
        {
            const auto input = matrix[row * size + column];
            const auto value = scale_ == 0 ? Complex{} : input / scale_;
            if ((input.real() != 0 && value.real() == 0) || (input.imag() != 0 && value.imag() == 0))
            { status_ = Status::DynamicRangeLoss; return false; }
            matrix_[offset(row, column)] = value;
            matrixNorm_ += std::norm(value);
        }
    matrixNorm_ = std::sqrt(matrixNorm_);
    if (scale_ == 0)
    {
        for (std::size_t i = 0; i < size; ++i) roots_[i] = {};
        status_ = Status::Ready;
    }
    else
    {
        status_ = Status::Running;
        if (size > 2)
        { reducing_ = true; start_ = index_ = 1; stage_ = Stage::Norm; }
    }
    return true;
}

void IncrementalComplexEigenvalues::reset() noexcept
{
    status_ = Status::Idle;
    stage_ = Stage::Check;
    size_ = active_ = iterations_ = sinceDeflation_ = consumedWork_ = 0;
    index_ = column_ = start_ = target_ = row_ = inner_ = 0;
    reducing_ = false;
    scale_ = matrixNorm_ = rowEnergy_ = norm_ = vectorEnergy_ = beta_ = 0;
    shift_ = phase_ = sum_ = {};
}

bool IncrementalComplexEigenvalues::canDeflate() const noexcept
{
    // Ahues--Tisseur two-stage criterion (see LAPACK ZLAHQR). A small lower
    // coupling alone is insufficient when the upper coupling is large.
    const auto magnitude = [](Complex value) { return std::abs(value.real()) + std::abs(value.imag()); };
    const auto a = matrix_[offset(active_ - 2, active_ - 2)];
    const auto b = matrix_[offset(active_ - 2, active_ - 1)];
    const auto c = matrix_[offset(active_ - 1, active_ - 2)];
    const auto d = matrix_[offset(active_ - 1, active_ - 1)];
    const auto lower = magnitude(c), upper = magnitude(b);
    const auto small = std::numeric_limits<double>::min() * (size_ / deflationTolerance);
    if (lower <= small) return true;
    auto diagonalScale = magnitude(a) + magnitude(d);
    if (diagonalScale == 0) diagonalScale = matrixNorm_;
    if (lower > deflationTolerance * diagonalScale) return false;
    const auto ab = std::max(lower, upper), ba = std::min(lower, upper);
    const auto aa = std::max(magnitude(d), magnitude(a - d));
    const auto bb = std::min(magnitude(d), magnitude(a - d));
    const auto scale = aa + ab;
    return scale == 0 || ba * (ab / scale) <= std::max(small, deflationTolerance * bb * (aa / scale));
}

void IncrementalComplexEigenvalues::deflate() noexcept
{
    const auto root = matrix_[offset(active_ - 1, active_ - 1)] * scale_;
    if (!finite(root)) { status_ = Status::NumericOverflow; return; }
    roots_[active_ - 1] = root;
    --active_;
    index_ = sinceDeflation_ = 0;
    rowEnergy_ = 0;
    if (active_ == 0) status_ = Status::Ready;
    stage_ = Stage::Check;
}

void IncrementalComplexEigenvalues::nextReflector() noexcept
{
    ++column_;
    if (reducing_ && column_ + 2 == active_)
    {
        reducing_ = false;
        index_ = 0;
        rowEnergy_ = 0;
        stage_ = Stage::Check;
    }
    else if (column_ == active_)
    {
        row_ = target_ = inner_ = 0;
        sum_ = {};
        stage_ = Stage::Product;
    }
    else
    {
        start_ = column_ + (reducing_ ? 1 : 0);
        index_ = start_;
        norm_ = 0;
        stage_ = Stage::Norm;
    }
}

void IncrementalComplexEigenvalues::advance(std::size_t& work) noexcept
{
    while (work != 0 && status_ == Status::Running)
    {
        --work;
        ++consumedWork_;
        switch (stage_)
        {
        case Stage::Check:
            if (active_ == 1) { deflate(); break; }
            rowEnergy_ = std::norm(matrix_[offset(active_ - 1, active_ - 2)]);
            if (canDeflate()) deflate();
            else if (iterations_ == maximumIterations) status_ = Status::DidNotConverge;
            else stage_ = Stage::Shift;
            break;
        case Stage::Shift:
        {
            ++iterations_;
            ++sinceDeflation_;
            const auto a = matrix_[offset(active_ - 2, active_ - 2)];
            const auto b = matrix_[offset(active_ - 2, active_ - 1)];
            const auto c = matrix_[offset(active_ - 1, active_ - 2)];
            const auto d = matrix_[offset(active_ - 1, active_ - 1)];
            if (sinceDeflation_ % 16 == 0)
                shift_ = d + Complex{.75, .25} * std::sqrt(rowEnergy_);
            else
            {
                const auto delta = (a - d) / 2.0;
                const auto root = std::sqrt(delta * delta + b * c);
                const auto first = delta + root, second = delta - root;
                const auto larger = std::abs(first) > std::abs(second) ? first : second;
                // The product of the two offsets is -b*c; avoid cancellation
                // in the smaller offset, which gives the root nearest d.
                shift_ = larger == Complex{} ? d : d - (b * c) / larger;
            }
            if (!finite(shift_)) { status_ = Status::NumericOverflow; break; }
            index_ = 0;
            stage_ = Stage::Setup;
            break;
        }
        case Stage::Setup:
        {
            const auto row = index_ / active_, column = index_ % active_;
            r_[offset(row, column)] = matrix_[offset(row, column)] - (row == column ? shift_ : Complex{});
            q_[offset(row, column)] = row == column ? Complex{1.0, 0.0} : Complex{};
            if (++index_ == active_ * active_)
            { column_ = start_ = index_ = 0; norm_ = 0; stage_ = Stage::Norm; }
            break;
        }
        case Stage::Norm:
            norm_ = std::hypot(norm_, std::abs((reducing_ ? matrix_ : r_)[offset(index_, column_)]));
            if (++index_ == active_)
            {
                if (!std::isfinite(norm_)) { status_ = Status::NumericOverflow; break; }
                if (norm_ == 0) { nextReflector(); break; }
                const auto leading = (reducing_ ? matrix_ : r_)[offset(start_, column_)];
                const auto absolute = std::abs(leading);
                phase_ = absolute == 0 ? Complex{1.0, 0.0} : leading / absolute;
                vectorEnergy_ = 0;
                index_ = start_;
                stage_ = Stage::Vector;
            }
            break;
        case Stage::Vector:
            reflector_[index_] = (reducing_ ? matrix_ : r_)[offset(index_, column_)] / norm_
                + (index_ == start_ ? phase_ : Complex{});
            vectorEnergy_ += std::norm(reflector_[index_]);
            if (++index_ == active_)
            {
                beta_ = 2.0 / vectorEnergy_;
                if (!std::isfinite(beta_)) { status_ = Status::NumericOverflow; break; }
                index_ = start_;
                target_ = column_;
                sum_ = {};
                stage_ = Stage::RDot;
            }
            break;
        case Stage::RDot:
            sum_ += std::conj(reflector_[index_]) * (reducing_ ? matrix_ : r_)[offset(index_, target_)];
            if (++index_ == active_) { index_ = start_; stage_ = Stage::RApply; }
            break;
        case Stage::RApply:
        {
            auto& value = (reducing_ ? matrix_ : r_)[offset(index_, target_)];
            value -= beta_ * reflector_[index_] * sum_;
            if (!finite(value)) { status_ = Status::NumericOverflow; break; }
            if (target_ == column_ && index_ > start_) value = {};
            if (++index_ == active_)
            {
                index_ = start_;
                sum_ = {};
                if (++target_ == active_) { target_ = 0; stage_ = Stage::QDot; }
                else stage_ = Stage::RDot;
            }
            break;
        }
        case Stage::QDot:
            sum_ += (reducing_ ? matrix_ : q_)[offset(target_, index_)] * reflector_[index_];
            if (++index_ == active_) { index_ = start_; stage_ = Stage::QApply; }
            break;
        case Stage::QApply:
        {
            auto& value = (reducing_ ? matrix_ : q_)[offset(target_, index_)];
            value -= beta_ * sum_ * std::conj(reflector_[index_]);
            if (!finite(value)) { status_ = Status::NumericOverflow; break; }
            if (++index_ == active_)
            {
                index_ = start_;
                sum_ = {};
                if (++target_ == active_) nextReflector();
                else stage_ = Stage::QDot;
            }
            break;
        }
        case Stage::Product:
            sum_ += r_[offset(row_, inner_)] * q_[offset(inner_, target_)];
            if (++inner_ == active_)
            {
                auto& value = matrix_[offset(row_, target_)];
                value = sum_ + (row_ == target_ ? shift_ : Complex{});
                if (!finite(value)) { status_ = Status::NumericOverflow; break; }
                if (row_ > target_ + 1) value = {}; // Known Hessenberg zeros.
                sum_ = {};
                inner_ = 0;
                if (++target_ == active_)
                {
                    target_ = 0;
                    if (++row_ == active_)
                    { index_ = 0; rowEnergy_ = 0; stage_ = Stage::Check; }
                }
            }
            break;
        }
    }
}

IncrementalComplexEigenvalues::Complex IncrementalComplexEigenvalues::eigenvalue(std::size_t index) const noexcept
{ return status_ == Status::Ready && index < size_ ? roots_[index] : Complex{}; }
}
