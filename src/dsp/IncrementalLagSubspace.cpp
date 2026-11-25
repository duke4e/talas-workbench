#include "talas/dsp/IncrementalLagSubspace.h"

#include <algorithm>
#include <cmath>

namespace talas::dsp
{
bool IncrementalLagSubspace::begin(std::span<const double> sequence, std::size_t columns) noexcept
{
    reset();
    if (sequence.empty() || sequence.size() > maximumSequenceSize || columns == 0
        || columns > maximumColumns || columns > (sequence.size() + 1) / 2)
    {
        status_ = Status::InvalidInput;
        return false;
    }
    for (const auto value : sequence)
    {
        if (!std::isfinite(value)) { status_ = Status::InvalidInput; return false; }
        scale_ = std::max(scale_, std::abs(value));
    }
    rows_ = sequence.size() - columns + 1;
    columns_ = columns;
    for (std::size_t i = 0; i < sequence.size(); ++i)
        sequence_[i] = scale_ == 0 ? 0 : sequence[i] / scale_;
    status_ = scale_ == 0 ? Status::Ready : Status::Running;
    return true;
}

void IncrementalLagSubspace::reset() noexcept
{
    status_ = Status::Idle;
    stage_ = Stage::Fill;
    rows_ = columns_ = rank_ = index_ = first_ = sweeps_ = rotations_ = consumedWork_ = 0;
    second_ = sortIndex_ = sortPosition_ = 1;
    scale_ = energy_ = alpha_ = beta_ = cross_ = sine_ = 0;
    cosine_ = 1;
}

std::size_t IncrementalLagSubspace::maximumWork() const noexcept
{
    if (columns_ == 0) return 0;
    return 2 * rows_ * columns_
        + maximumSweeps * columns_ * (columns_ - 1) / 2 * (2 * rows_ + 1)
        + columns_ * columns_ + 2 * columns_ + 16;
}

void IncrementalLagSubspace::beginNorms() noexcept
{
    stage_ = Stage::Norm;
    index_ = first_ = 0;
    alpha_ = 0;
}

void IncrementalLagSubspace::nextPair() noexcept
{
    index_ = 0;
    alpha_ = beta_ = cross_ = 0;
    stage_ = Stage::Dot;
    if (++second_ < columns_) return;
    second_ = ++first_ + 1;
    if (second_ < columns_) return;
    ++sweeps_;
    if (rotations_ == 0) { beginNorms(); return; }
    if (sweeps_ == maximumSweeps) { status_ = Status::DidNotConverge; return; }
    rotations_ = first_ = 0;
    second_ = 1;
}

void IncrementalLagSubspace::advance(std::size_t& work) noexcept
{
    while (work != 0 && status_ == Status::Running)
    {
        --work;
        ++consumedWork_;
        switch (stage_)
        {
        case Stage::Fill:
        {
            const auto value = sequence_[index_ % rows_ + index_ / rows_];
            values_[index_] = value;
            energy_ += value * value;
            if (index_ % rows_ == 0) norms_[index_ / rows_] = 0;
            norms_[index_ / rows_] += value * value;
            if (++index_ == rows_ * columns_)
            {
                index_ = 0;
                if (columns_ == 1) beginNorms();
                else stage_ = Stage::Dot;
            }
            break;
        }
        case Stage::Dot:
        {
            if (index_ == 0)
            {
                alpha_ = norms_[first_];
                beta_ = norms_[second_];
                // Several small columns can jointly carry an admitted mode.
                // Limit their collective Frobenius norm to the final rank
                // floor divided by sqrt(columns). Cached row sums avoid
                // scanning inactive pairs; final rank admission is unchanged.
                const auto floor = energy_ * relativeSingularFloor * relativeSingularFloor
                    / (columns_ * columns_);
                if (alpha_ <= floor || beta_ <= floor) { nextPair(); break; }
            }
            const auto a = values_[first_ * rows_ + index_];
            const auto b = values_[second_ * rows_ + index_];
            cross_ += a * b;
            if (++index_ == rows_) { index_ = 0; stage_ = Stage::Decide; }
            break;
        }
        case Stage::Decide:
        {
            if (std::abs(cross_) <= correlationTolerance * std::sqrt(alpha_) * std::sqrt(beta_))
            { nextPair(); break; }
            const auto zeta = (beta_ - alpha_) / (2 * cross_);
            const auto tangent = std::copysign(1.0 / (std::abs(zeta) + std::hypot(1.0, zeta)), zeta);
            cosine_ = 1.0 / std::sqrt(1.0 + tangent * tangent);
            sine_ = tangent * cosine_;
            alpha_ = beta_ = 0;
            ++rotations_;
            stage_ = Stage::Rotate;
            break;
        }
        case Stage::Rotate:
        {
            auto& a = values_[first_ * rows_ + index_];
            auto& b = values_[second_ * rows_ + index_];
            const auto previous = a;
            a = cosine_ * a - sine_ * b;
            b = sine_ * previous + cosine_ * b;
            alpha_ += a * a;
            beta_ += b * b;
            if (!std::isfinite(a) || !std::isfinite(b))
            { status_ = Status::NumericOverflow; break; }
            if (++index_ == rows_)
            {
                norms_[first_] = alpha_;
                norms_[second_] = beta_;
                nextPair();
            }
            break;
        }
        case Stage::Norm:
        {
            const auto value = values_[first_ * rows_ + index_];
            alpha_ += value * value;
            if (++index_ == rows_)
            {
                norms_[first_] = std::sqrt(alpha_);
                order_[first_] = first_;
                if (!std::isfinite(norms_[first_] * scale_))
                { status_ = Status::NumericOverflow; break; }
                index_ = 0;
                alpha_ = 0;
                if (++first_ == columns_)
                { stage_ = Stage::Sort; sortIndex_ = sortPosition_ = 1; }
            }
            break;
        }
        case Stage::Sort:
            if (sortIndex_ == columns_)
            {
                rank_ = 0;
                index_ = 0;
                stage_ = Stage::Rank;
            }
            else if (sortPosition_ != 0
                     && norms_[order_[sortPosition_]] > norms_[order_[sortPosition_ - 1]])
            {
                std::swap(order_[sortPosition_], order_[sortPosition_ - 1]);
                --sortPosition_;
            }
            else sortPosition_ = ++sortIndex_;
            break;
        case Stage::Rank:
            if (norms_[order_[index_]] > std::sqrt(energy_) * relativeSingularFloor) ++rank_;
            if (++index_ == columns_) status_ = Status::Ready;
            break;
        }
    }
}

double IncrementalLagSubspace::singularValue(std::size_t mode) const noexcept
{
    return status_ == Status::Ready && mode < rank_ ? norms_[order_[mode]] * scale_ : 0;
}

double IncrementalLagSubspace::basis(std::size_t mode, std::size_t row) const noexcept
{
    return status_ == Status::Ready && mode < rank_ && row < rows_
        ? values_[order_[mode] * rows_ + row] / norms_[order_[mode]] : 0;
}
}
