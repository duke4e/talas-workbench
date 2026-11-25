#include "talas/dsp/IncrementalSubspaceModes.h"
#include "talas/dsp/IncrementalForwardBackwardSubspace.h"

#include <algorithm>
#include <cmath>

namespace talas::dsp
{
template <class Subspace>
bool IncrementalSubspaceModes::beginSubspace(const Subspace& subspace, std::size_t modes) noexcept
{
    reset();
    if (subspace.status() != Subspace::Status::Ready || modes < 2
        || modes > maximumModes || modes % 2 != 0 || modes > subspace.rank() || subspace.rows() <= modes)
    { status_ = Status::InvalidInput; return false; }
    rows_ = subspace.rows();
    modes_ = modes;
    for (std::size_t row = 0; row < rows_; ++row)
        for (std::size_t column = 0; column < modes_; ++column)
            basis_[offset(row, column)] = subspace.basis(column, row);
    for (std::size_t row = 0; row < modes_; ++row)
        for (std::size_t column = 0; column < modes_; ++column)
            gram_[offset(row, column)] = shift_[offset(row, column)] = 0;
    status_ = Status::Running;
    return true;
}

bool IncrementalSubspaceModes::begin(const IncrementalLagSubspace& subspace, std::size_t modes) noexcept
{ return beginSubspace(subspace, modes); }

bool IncrementalSubspaceModes::begin(const IncrementalForwardBackwardSubspace& subspace, std::size_t modes) noexcept
{ return beginSubspace(subspace, modes); }

void IncrementalSubspaceModes::reset() noexcept
{
    status_ = Status::Idle;
    stage_ = Stage::Accumulate;
    rows_ = modes_ = consumedWork_ = row_ = column_ = index_ = pivot_ = selected_ = 0;
    gramScale_ = largest_ = divisor_ = factor_ = 0;
    eigenvalues_.reset();
}

std::size_t IncrementalSubspaceModes::maximumWork() const noexcept
{
    return modes_ == 0 ? 0 : rows_ * modes_ * modes_ + 4 * modes_ * modes_ * modes_
        + 8 * modes_ * modes_ + 8 * modes_ + IncrementalComplexEigenvalues::maximumWorkForSize(modes_);
}

void IncrementalSubspaceModes::advance(std::size_t& work) noexcept
{
    while (work != 0 && status_ == Status::Running)
    {
        if (stage_ == Stage::Eigen)
        {
            const auto before = work;
            eigenvalues_.advance(work);
            consumedWork_ += before - work;
            switch (eigenvalues_.status())
            {
            case IncrementalComplexEigenvalues::Status::Ready: status_ = Status::Ready; break;
            case IncrementalComplexEigenvalues::Status::Running: break;
            case IncrementalComplexEigenvalues::Status::DidNotConverge: status_ = Status::DidNotConverge; break;
            default: status_ = Status::NumericOverflow; break;
            }
            continue;
        }
        --work;
        ++consumedWork_;
        switch (stage_)
        {
        case Stage::Accumulate:
        {
            auto& gram = gram_[offset(row_, column_)];
            auto& shifted = shift_[offset(row_, column_)];
            gram += basis_[offset(index_, row_)] * basis_[offset(index_, column_)];
            shifted += basis_[offset(index_, row_)] * basis_[offset(index_ + 1, column_)];
            if (++index_ == rows_ - 1)
            {
                gramScale_ = std::max(gramScale_, std::abs(gram));
                index_ = 0;
                if (++column_ == modes_)
                {
                    column_ = 0;
                    if (++row_ == modes_) { row_ = 0; stage_ = Stage::FindPivot; }
                }
            }
            break;
        }
        case Stage::FindPivot:
            if (const auto value = std::abs(gram_[offset(row_, pivot_)]); value > largest_)
            { largest_ = value; selected_ = row_; }
            if (++row_ == modes_)
            {
                // Conservative admission threshold, not a condition-number estimate.
                if (largest_ <= 1e-12 * gramScale_) { status_ = Status::SingularShift; break; }
                column_ = 0;
                stage_ = Stage::Swap;
            }
            break;
        case Stage::Swap:
            std::swap(gram_[offset(pivot_, column_)], gram_[offset(selected_, column_)]);
            std::swap(shift_[offset(pivot_, column_)], shift_[offset(selected_, column_)]);
            if (++column_ == modes_)
            { column_ = 0; divisor_ = gram_[offset(pivot_, pivot_)]; stage_ = Stage::Normalize; }
            break;
        case Stage::Normalize:
            gram_[offset(pivot_, column_)] /= divisor_;
            shift_[offset(pivot_, column_)] /= divisor_;
            if (!std::isfinite(gram_[offset(pivot_, column_)]) || !std::isfinite(shift_[offset(pivot_, column_)]))
            { status_ = Status::NumericOverflow; break; }
            if (++column_ == modes_)
            { gram_[offset(pivot_, pivot_)] = 1; row_ = 0; stage_ = Stage::SelectRow; }
            break;
        case Stage::SelectRow:
            if (row_ == modes_) stage_ = Stage::NextPivot;
            else if (row_ == pivot_) ++row_;
            else
            {
                factor_ = gram_[offset(row_, pivot_)];
                column_ = 0;
                stage_ = Stage::Eliminate;
            }
            break;
        case Stage::Eliminate:
        {
            auto& gram = gram_[offset(row_, column_)];
            auto& shifted = shift_[offset(row_, column_)];
            gram -= factor_ * gram_[offset(pivot_, column_)];
            shifted -= factor_ * shift_[offset(pivot_, column_)];
            if (!std::isfinite(gram) || !std::isfinite(shifted))
            { status_ = Status::NumericOverflow; break; }
            if (column_ == pivot_) gram = 0;
            if (++column_ == modes_) { ++row_; stage_ = Stage::SelectRow; }
            break;
        }
        case Stage::NextPivot:
            if (++pivot_ == modes_) { index_ = 0; stage_ = Stage::Pack; }
            else { selected_ = row_ = pivot_; largest_ = 0; stage_ = Stage::FindPivot; }
            break;
        case Stage::Pack:
            packed_[index_] = shift_[offset(index_ / modes_, index_ % modes_)];
            if (++index_ == modes_ * modes_)
            {
                if (!eigenvalues_.begin(std::span{packed_}.first(index_), modes_))
                    status_ = Status::NumericOverflow;
                else if (eigenvalues_.status() == IncrementalComplexEigenvalues::Status::Ready)
                    status_ = Status::Ready;
                else stage_ = Stage::Eigen;
            }
            break;
        case Stage::Eigen: break; // Handled before charging a parent work unit.
        }
    }
}

IncrementalSubspaceModes::Complex IncrementalSubspaceModes::eigenvalue(std::size_t index) const noexcept
{ return status_ == Status::Ready ? eigenvalues_.eigenvalue(index) : Complex{}; }

double IncrementalSubspaceModes::shift(std::size_t row, std::size_t column) const noexcept
{ return status_ == Status::Ready && row < modes_ && column < modes_ ? shift_[offset(row, column)] : 0; }
}
