#include "talas/dsp/IncrementalForwardBackwardSubspace.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace talas::dsp
{
namespace
{
constexpr double inverseSqrtTwo = 0.707106781186547524400844362104849039;
constexpr double sqrtTwo = 1.414213562373095048801688724209698079;
}

bool IncrementalForwardBackwardSubspace::begin(std::span<const double> differences) noexcept
{
    reset();
    if (differences.size() != differenceCount)
    {
        status_ = Status::InvalidInput;
        return false;
    }

    for (const auto value : differences)
    {
        if (!std::isfinite(value))
        {
            status_ = Status::InvalidInput;
            return false;
        }
        scale_ = std::max(scale_, std::abs(value));
    }

    if (scale_ == 0.0)
    {
        status_ = Status::Ready;
        return true;
    }

    for (std::size_t i = 0; i < differenceCount; ++i)
    {
        sequence_[i] = differences[i] / scale_;
        if (!std::isfinite(sequence_[i]) || (differences[i] != 0.0 && sequence_[i] == 0.0))
        {
            status_ = Status::NumericOverflow;
            return false;
        }
    }

    status_ = Status::Running;
    stage_ = Stage::Energy;
    return true;
}

void IncrementalForwardBackwardSubspace::reset() noexcept
{
    status_ = Status::Idle;
    stage_ = Stage::Energy;
    rank_ = consumedWork_ = energyIndex_ = projectionIndex_ = 0;
    factor_ = column_ = row_ = rotationIndex_ = 0;
    first_ = 0;
    second_ = 1;
    index_ = 0;
    sweeps_.fill(0);
    rotations_.fill(0);
    rotationsInSweep_ = sortIndex_ = sortPosition_ = 1;
    rankIndex_ = 0;
    scale_ = energy_ = alpha_ = beta_ = cross_ = sine_ = 0.0;
    cosine_ = 1.0;
}

std::size_t IncrementalForwardBackwardSubspace::maximumWork() const noexcept
{
    return status_ == Status::Idle || status_ == Status::InvalidInput
        ? 0 : maximumWorkForShape();
}

std::size_t IncrementalForwardBackwardSubspace::factorColumns() const noexcept
{
    return factor_ == 0 ? evenColumns : oddColumns;
}

double& IncrementalForwardBackwardSubspace::factorValue(
    std::size_t factor, std::size_t column, std::size_t row) noexcept
{
    return factor == 0 ? evenValues_[column * hankelColumns + row]
                       : oddValues_[column * hankelColumns + row];
}

double IncrementalForwardBackwardSubspace::factorValue(
    std::size_t factor, std::size_t column, std::size_t row) const noexcept
{
    return factor == 0 ? evenValues_[column * hankelColumns + row]
                       : oddValues_[column * hankelColumns + row];
}

double& IncrementalForwardBackwardSubspace::factorNorm(std::size_t factor, std::size_t column) noexcept
{
    return factor == 0 ? evenNorms_[column] : oddNorms_[column];
}

double IncrementalForwardBackwardSubspace::factorNorm(std::size_t factor, std::size_t column) const noexcept
{
    return factor == 0 ? evenNorms_[column] : oddNorms_[column];
}

double& IncrementalForwardBackwardSubspace::factorRotation(
    std::size_t factor, std::size_t row, std::size_t column) noexcept
{
    return factor == 0 ? evenRotations_[row * evenColumns + column]
                       : oddRotations_[row * oddColumns + column];
}

double IncrementalForwardBackwardSubspace::factorRotation(
    std::size_t factor, std::size_t row, std::size_t column) const noexcept
{
    return factor == 0 ? evenRotations_[row * evenColumns + column]
                       : oddRotations_[row * oddColumns + column];
}

void IncrementalForwardBackwardSubspace::finishFactorNorms() noexcept
{
    stage_ = Stage::FinalNorm;
    column_ = row_ = 0;
    alpha_ = 0.0;
}

void IncrementalForwardBackwardSubspace::nextPair() noexcept
{
    index_ = 0;
    alpha_ = beta_ = cross_ = 0.0;
    stage_ = Stage::Dot;
    if (++second_ < factorColumns()) return;
    second_ = ++first_ + 1;
    if (second_ < factorColumns()) return;

    ++sweeps_[factor_];
    if (rotationsInSweep_ == 0)
    {
        finishFactorNorms();
        return;
    }
    if (sweeps_[factor_] == maximumSweeps)
    {
        status_ = Status::DidNotConverge;
        return;
    }
    rotationsInSweep_ = 0;
    first_ = 0;
    second_ = 1;
}

void IncrementalForwardBackwardSubspace::advance(std::size_t& work) noexcept
{
    while (work != 0 && status_ == Status::Running)
    {
        --work;
        ++consumedWork_;
        switch (stage_)
        {
        case Stage::Energy:
        {
            constexpr auto blockSize = rowsCount * hankelColumns;
            const auto block = energyIndex_ / blockSize;
            const auto inBlock = energyIndex_ % blockSize;
            const auto row = inBlock % rowsCount;
            const auto column = inBlock / rowsCount;
            const auto sourceRow = block == 0 ? row : rowsCount - 1 - row;
            const auto value = sequence_[sourceRow + column];
            energy_ += value * value;
            if (!std::isfinite(energy_)) { status_ = Status::NumericOverflow; break; }
            if (++energyIndex_ == 2 * blockSize)
            {
                if (energy_ == 0.0)
                {
                    status_ = Status::Ready;
                    rank_ = 0;
                    break;
                }
                stage_ = Stage::Projection;
                projectionIndex_ = 0;
            }
            break;
        }
        case Stage::Projection:
        {
            const auto row = projectionIndex_ / hankelColumns;
            const auto column = projectionIndex_ % hankelColumns;
            if (row < oddColumns)
            {
                const auto top = sequence_[row + column];
                const auto bottom = sequence_[rowsCount - 1 - row + column];
                factorValue(0, row, column) = (top + bottom) * inverseSqrtTwo;
                factorValue(1, row, column) = (top - bottom) * inverseSqrtTwo;
                if (!std::isfinite(factorValue(0, row, column))
                    || !std::isfinite(factorValue(1, row, column)))
                { status_ = Status::NumericOverflow; break; }
            }
            else
            {
                factorValue(0, evenColumns - 1, column) = sequence_[oddColumns + column];
                if (!std::isfinite(factorValue(0, evenColumns - 1, column)))
                { status_ = Status::NumericOverflow; break; }
            }
            if (++projectionIndex_ == rowsCount * hankelColumns)
            {
                factor_ = 0;
                column_ = row_ = 0;
                alpha_ = 0.0;
                stage_ = Stage::FactorNorm;
            }
            break;
        }
        case Stage::FactorNorm:
        {
            const auto value = factorValue(factor_, column_, row_);
            alpha_ += value * value;
            if (!std::isfinite(alpha_)) { status_ = Status::NumericOverflow; break; }
            if (++row_ == hankelColumns)
            {
                factorNorm(factor_, column_) = alpha_;
                alpha_ = 0.0;
                row_ = 0;
                if (++column_ == factorColumns())
                {
                    column_ = 0;
                    rotationIndex_ = 0;
                    stage_ = Stage::FactorRotationInit;
                }
            }
            break;
        }
        case Stage::FactorRotationInit:
        {
            const auto columns = factorColumns();
            const auto row = rotationIndex_ / columns;
            const auto column = rotationIndex_ % columns;
            factorRotation(factor_, row, column) = row == column ? 1.0 : 0.0;
            if (++rotationIndex_ == columns * columns)
            {
                first_ = 0;
                second_ = 1;
                index_ = 0;
                rotationsInSweep_ = 0;
                cross_ = 0.0;
                stage_ = Stage::Dot;
            }
            break;
        }
        case Stage::Dot:
        {
            if (index_ == 0)
            {
                alpha_ = factorNorm(factor_, first_);
                beta_ = factorNorm(factor_, second_);
                const auto pairFloor = energy_ * std::numeric_limits<double>::epsilon()
                    * std::numeric_limits<double>::epsilon();
                if (alpha_ <= pairFloor || beta_ <= pairFloor)
                {
                    nextPair();
                    break;
                }
            }
            const auto a = factorValue(factor_, first_, index_);
            const auto b = factorValue(factor_, second_, index_);
            cross_ += a * b;
            if (!std::isfinite(cross_)) { status_ = Status::NumericOverflow; break; }
            if (++index_ == hankelColumns)
            {
                index_ = 0;
                stage_ = Stage::Decide;
            }
            break;
        }
        case Stage::Decide:
        {
            const auto threshold = correlationTolerance * std::sqrt(alpha_) * std::sqrt(beta_);
            if (!std::isfinite(threshold)) { status_ = Status::NumericOverflow; break; }
            if (std::abs(cross_) <= threshold)
            {
                nextPair();
                break;
            }
            const auto zeta = (beta_ - alpha_) / (2.0 * cross_);
            const auto denominator = std::abs(zeta) + std::hypot(1.0, zeta);
            if (!std::isfinite(zeta) || !std::isfinite(denominator))
            { status_ = Status::NumericOverflow; break; }
            const auto tangent = std::copysign(1.0 / denominator, zeta);
            cosine_ = 1.0 / std::sqrt(1.0 + tangent * tangent);
            sine_ = tangent * cosine_;
            if (!std::isfinite(cosine_) || !std::isfinite(sine_))
            { status_ = Status::NumericOverflow; break; }
            alpha_ = beta_ = 0.0;
            index_ = 0;
            ++rotationsInSweep_;
            ++rotations_[factor_];
            stage_ = Stage::RotateData;
            break;
        }
        case Stage::RotateData:
        {
            auto& a = factorValue(factor_, first_, index_);
            auto& b = factorValue(factor_, second_, index_);
            const auto previous = a;
            a = cosine_ * a - sine_ * b;
            b = sine_ * previous + cosine_ * b;
            alpha_ += a * a;
            beta_ += b * b;
            if (!std::isfinite(a) || !std::isfinite(b)
                || !std::isfinite(alpha_) || !std::isfinite(beta_))
            { status_ = Status::NumericOverflow; break; }
            if (++index_ == hankelColumns)
            {
                factorNorm(factor_, first_) = alpha_;
                factorNorm(factor_, second_) = beta_;
                index_ = 0;
                stage_ = Stage::RotateVectors;
            }
            break;
        }
        case Stage::RotateVectors:
        {
            auto& a = factorRotation(factor_, index_, first_);
            auto& b = factorRotation(factor_, index_, second_);
            const auto previous = a;
            a = cosine_ * a - sine_ * b;
            b = sine_ * previous + cosine_ * b;
            if (!std::isfinite(a) || !std::isfinite(b))
            { status_ = Status::NumericOverflow; break; }
            if (++index_ == factorColumns())
            {
                index_ = 0;
                nextPair();
            }
            break;
        }
        case Stage::FinalNorm:
        {
            const auto value = factorValue(factor_, column_, row_);
            alpha_ += value * value;
            if (!std::isfinite(alpha_)) { status_ = Status::NumericOverflow; break; }
            if (++row_ == hankelColumns)
            {
                const auto code = (factor_ == 0 ? 0 : evenColumns) + column_;
                singularValues_[code] = sqrtTwo * std::sqrt(alpha_);
                order_[code] = code;
                if (!std::isfinite(singularValues_[code])
                    || !std::isfinite(singularValues_[code] * scale_))
                { status_ = Status::NumericOverflow; break; }
                alpha_ = 0.0;
                row_ = 0;
                if (++column_ == factorColumns())
                {
                    if (factor_ == 0)
                    {
                        factor_ = 1;
                        column_ = 0;
                        row_ = 0;
                        alpha_ = 0.0;
                        stage_ = Stage::FactorNorm;
                    }
                    else
                    {
                        sortIndex_ = sortPosition_ = 1;
                        stage_ = Stage::Sort;
                    }
                }
            }
            break;
        }
        case Stage::Sort:
            if (sortIndex_ == maximumModes)
            {
                rankIndex_ = 0;
                rank_ = 0;
                stage_ = Stage::Rank;
            }
            else if (sortPosition_ != 0
                     && singularValues_[order_[sortPosition_]] > singularValues_[order_[sortPosition_ - 1]])
            {
                std::swap(order_[sortPosition_], order_[sortPosition_ - 1]);
                --sortPosition_;
            }
            else sortPosition_ = ++sortIndex_;
            break;
        case Stage::Rank:
            if (singularValues_[order_[rankIndex_]] > std::sqrt(energy_) * relativeSingularFloor)
                ++rank_;
            if (++rankIndex_ == maximumModes) status_ = Status::Ready;
            break;
        }
    }
}

double IncrementalForwardBackwardSubspace::singularValue(std::size_t mode) const noexcept
{
    return status_ == Status::Ready && mode < rank_
        ? singularValues_[order_[mode]] * scale_ : 0.0;
}

double IncrementalForwardBackwardSubspace::basis(std::size_t mode, std::size_t row) const noexcept
{
    if (status_ != Status::Ready || mode >= rank_ || row >= rowsCount) return 0.0;
    const auto code = order_[mode];
    if (code < evenColumns)
    {
        const auto coordinate = std::min(row, rowsCount - 1 - row);
        const auto value = factorRotation(0, coordinate, code);
        return coordinate == evenColumns - 1 ? value : value * inverseSqrtTwo;
    }

    if (row == oddColumns) return 0.0;
    const auto coordinate = std::min(row, rowsCount - 1 - row);
    const auto sign = row < oddColumns ? 1.0 : -1.0;
    return sign * factorRotation(1, coordinate, code - evenColumns) * inverseSqrtTwo;
}
}
