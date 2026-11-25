#include "talas/dsp/IncrementalSinusoidFit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace talas::dsp
{
namespace { constexpr auto tau = 2 * std::numbers::pi; }

bool IncrementalSinusoidFit::finite(double value) noexcept
{
    if (std::isfinite(value)) return true;
    status_ = Status::NumericOverflow;
    return false;
}

bool IncrementalSinusoidFit::begin(std::span<const double> samples, double sampleRate,
                                  std::span<const double> frequencies, bool refine, bool validate) noexcept
{
    reset();
    if (samples.size() < 32 || samples.size() > maximumSamples || samples.size() % 4 != 0
        || frequencies.size() > maximumPartials || !std::isfinite(sampleRate) || sampleRate <= 0)
    { status_ = Status::InvalidInput; return false; }
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        if (!std::isfinite(frequencies[i]) || frequencies[i] <= 0 || frequencies[i] >= sampleRate / 2
            || (i != 0 && frequencies[i] - frequencies[i - 1] < 1e-6))
        { status_ = Status::InvalidInput; return false; }
    std::memcpy(samples_.data(), samples.data(), samples.size_bytes());
    samplesCount_ = samples.size();
    trainingCount_ = 3 * samplesCount_ / 4;
    partials_ = frequencies.size();
    sampleRate_ = sampleRate;
    refine_ = refine;
    validate_ = validate;
    for (std::size_t i = 0; i < partials_; ++i)
        initialFrequencies_[i] = frequencies_[i] = trialFrequencies_[i] = frequencies[i];
    status_ = Status::Running;
    return true;
}

void IncrementalSinusoidFit::reset() noexcept
{
    status_ = Status::Idle; stage_ = Stage::Validate; pass_ = Pass::Initial;
    samplesCount_ = trainingCount_ = partials_ = columns_ = index_ = component_ = pivot_ = column_ = 0;
    inverseIndex_ = solveRow_ = refinements_ = consumedWork_ = 0;
    refine_ = validating_ = false;
    validate_ = true;
    sampleRate_ = scale_ = mean_ = variance_ = 0; normalizer_ = 1;
    cosine_ = sine_ = predicted_ = sum_ = norm_ = inverseNorm_ = currentCondition_ = bestCondition_ = 0;
    stepScale_ = errorEnergy_ = maximumError_ = bestEnergy_ = trainingError_ = validationError_ = ratio_ = constantError_ = 0;
}

std::size_t IncrementalSinusoidFit::maximumWork() const noexcept
{
    return maximumWorkForSize(samplesCount_, partials_);
}

void IncrementalSinusoidFit::startPass(Pass pass) noexcept
{
    pass_ = pass;
    columns_ = 1 + (pass == Pass::Jacobian ? 3 : 2) * partials_;
    index_ = 0; stage_ = Stage::Clear;
}

void IncrementalSinusoidFit::startError(bool validation) noexcept
{
    validating_ = validation;
    index_ = validation ? trainingCount_ : 0;
    errorEnergy_ = maximumError_ = 0;
    stage_ = Stage::ErrorStart;
}

void IncrementalSinusoidFit::startValidation() noexcept
{
    if (validate_) startError(true);
    else { component_ = 0; stage_ = Stage::Finalize; }
}

void IncrementalSinusoidFit::rejectSolve() noexcept
{
    if (pass_ == Pass::Initial) status_ = Status::IllConditioned;
    else startValidation(); // Preserve the last completed fit.
}

void IncrementalSinusoidFit::nextRotation() noexcept
{
    if (++pivot_ != columns_) { stage_ = Stage::Rotation; return; }
    if (++index_ != trainingCount_) stage_ = Stage::RowStart;
    else { index_ = 0; norm_ = 0; stage_ = Stage::ConditionNorm; }
}

void IncrementalSinusoidFit::advance(std::size_t& work) noexcept
{
    while (work != 0 && status_ == Status::Running)
    {
        --work; ++consumedWork_;
        switch (stage_)
        {
        case Stage::Validate:
            if (!std::isfinite(samples_[index_])) { status_ = Status::InvalidInput; break; }
            if (index_ < trainingCount_) scale_ = std::max(scale_, std::abs(samples_[index_]));
            if (++index_ == (validate_ ? samplesCount_ : trainingCount_))
            { if (scale_ == 0) scale_ = 1; index_ = 0; stage_ = Stage::NormalizeStatistics; }
            break;
        case Stage::NormalizeStatistics:
        {
            const auto original = samples_[index_];
            samples_[index_] /= scale_;
            if (!finite(samples_[index_])) break;
            if (original != 0 && samples_[index_] == 0) { status_ = Status::DynamicRangeLoss; break; }
            if (index_ < trainingCount_)
            {
                const auto difference = samples_[index_] - mean_;
                mean_ += difference / (index_ + 1);
                variance_ += difference * (samples_[index_] - mean_);
            }
            if (++index_ == (validate_ ? samplesCount_ : trainingCount_))
            {
                // Physical1e-12 noise floor, expressed in normalized sample units.
                normalizer_ = std::max(std::sqrt(std::max(0.0, variance_) / trainingCount_), 1e-12 / scale_);
                if (partials_ == 0)
                {
                    trialCoefficients_[0] = mean_; bestCondition_ = 1;
                    startError(false);
                }
                else startPass(Pass::Initial);
            }
            break;
        }
        case Stage::Clear:
            qr_[offset(index_ / (columns_ + 1), index_ % (columns_ + 1))] = 0;
            if (++index_ == columns_ * (columns_ + 1)) { index_ = 0; stage_ = Stage::RowStart; }
            break;
        case Stage::RowStart:
            rowValues_[0] = 1;
            rowValues_[columns_] = samples_[index_];
            predicted_ = pass_ == Pass::Jacobian ? coefficients_[0] : 0;
            component_ = 0; stage_ = Stage::Basis;
            break;
        case Stage::Basis:
        {
            const auto& frequencies = pass_ == Pass::Jacobian ? frequencies_ : trialFrequencies_;
            const auto angle = tau * (frequencies[component_] / sampleRate_) * index_;
            const auto c = std::cos(angle), s = std::sin(angle);
            rowValues_[1 + component_] = c; rowValues_[1 + partials_ + component_] = s;
            if (pass_ == Pass::Jacobian)
            {
                const auto a = coefficients_[1 + component_], b = coefficients_[1 + partials_ + component_];
                predicted_ += a * c + b * s;
                rowValues_[1 + 2 * partials_ + component_] = tau * (static_cast<double>(index_) / trainingCount_)
                    * (-a * s + b * c);
            }
            if (++component_ == partials_)
            {
                if (pass_ == Pass::Jacobian) rowValues_[columns_] -= predicted_;
                pivot_ = 0; stage_ = Stage::Rotation;
            }
            break;
        }
        case Stage::Rotation:
        {
            const auto diagonal = qr_[offset(pivot_, pivot_)], incoming = rowValues_[pivot_];
            const auto magnitude = std::hypot(diagonal, incoming);
            if (!finite(magnitude)) break;
            if (magnitude == 0) { nextRotation(); break; }
            cosine_ = diagonal / magnitude; sine_ = incoming / magnitude;
            column_ = pivot_; stage_ = Stage::Rotate;
            break;
        }
        case Stage::Rotate:
        {
            auto& top = qr_[offset(pivot_, column_)];
            auto& bottom = rowValues_[column_];
            const auto oldTop = top;
            top = cosine_ * oldTop + sine_ * bottom;
            bottom = -sine_ * oldTop + cosine_ * bottom;
            if (!finite(top) || !finite(bottom)) break;
            if (column_ == pivot_) bottom = 0;
            if (++column_ > columns_) nextRotation();
            break;
        }
        case Stage::ConditionNorm:
        {
            const auto value = qr_[offset(index_ / columns_, index_ % columns_)];
            norm_ = std::hypot(norm_, value);
            if (++index_ == columns_ * columns_)
            { inverseIndex_ = 0; inverseNorm_ = 0; stage_ = Stage::InverseStart; }
            break;
        }
        case Stage::InverseStart:
            solveRow_ = columns_ - 1; column_ = columns_; sum_ = solveRow_ == inverseIndex_ ? 1 : 0;
            stage_ = Stage::InverseDivide;
            break;
        case Stage::InverseSum:
            sum_ -= qr_[offset(solveRow_, column_)] * inverseColumn_[column_];
            if (++column_ == columns_) stage_ = Stage::InverseDivide;
            break;
        case Stage::InverseDivide:
        {
            const auto diagonal = qr_[offset(solveRow_, solveRow_)];
            if (diagonal == 0) { rejectSolve(); break; }
            const auto value = sum_ / diagonal;
            inverseColumn_[solveRow_] = value;
            inverseNorm_ = std::hypot(inverseNorm_, value);
            if (!std::isfinite(inverseNorm_) || norm_ * inverseNorm_ > maximumConditionBound)
            { rejectSolve(); break; }
            if (solveRow_ != 0)
            {
                --solveRow_; column_ = solveRow_ + 1; sum_ = solveRow_ == inverseIndex_ ? 1 : 0;
                stage_ = Stage::InverseSum;
            }
            else if (++inverseIndex_ == columns_)
            { currentCondition_ = norm_ * inverseNorm_; stage_ = Stage::SolveStart; }
            else stage_ = Stage::InverseStart;
            break;
        }
        case Stage::SolveStart:
            solveRow_ = columns_ - 1; sum_ = qr_[offset(solveRow_, columns_)]; stage_ = Stage::SolveDivide;
            break;
        case Stage::SolveSum:
            sum_ -= qr_[offset(solveRow_, column_)] * solution_[column_];
            if (++column_ == columns_) stage_ = Stage::SolveDivide;
            break;
        case Stage::SolveDivide:
            solution_[solveRow_] = sum_ / qr_[offset(solveRow_, solveRow_)];
            if (!finite(solution_[solveRow_])) break;
            if (solveRow_ != 0)
            { --solveRow_; column_ = solveRow_ + 1; sum_ = qr_[offset(solveRow_, columns_)]; stage_ = Stage::SolveSum; }
            else { component_ = 0; stepScale_ = 0; stage_ = pass_ == Pass::Jacobian ? Stage::StepSize : Stage::Store; }
            break;
        case Stage::Store:
            trialCoefficients_[component_] = solution_[component_];
            if (++component_ == 1 + 2 * partials_) startError(false);
            break;
        case Stage::StepSize:
            stepScale_ = std::max(stepScale_, std::abs(solution_[1 + 2 * partials_ + component_]));
            if (++component_ == partials_)
            { stepScale_ = stepScale_ == 0 ? 1 : std::min(1.0, .5 / stepScale_); component_ = 0; stage_ = Stage::TrialFrequency; }
            break;
        case Stage::TrialFrequency:
        {
            const auto bin = sampleRate_ / trainingCount_;
            const auto trial = frequencies_[component_] + stepScale_ * solution_[1 + 2 * partials_ + component_] * bin;
            if (!std::isfinite(trial) || trial <= 0 || trial >= sampleRate_ / 2
                || std::abs(trial - initialFrequencies_[component_]) > bin
                || (component_ != 0 && trial - trialFrequencies_[component_ - 1] < 1e-6))
            { startValidation(); break; }
            trialFrequencies_[component_] = trial;
            if (++component_ == partials_) startPass(Pass::Trial);
            break;
        }
        case Stage::ErrorStart:
            predicted_ = (validating_ ? coefficients_ : trialCoefficients_)[0];
            component_ = 0; stage_ = partials_ == 0 ? Stage::ErrorFinish : Stage::ErrorBasis;
            break;
        case Stage::ErrorBasis:
        {
            const auto& frequencies = validating_ ? frequencies_ : trialFrequencies_;
            const auto& coefficients = validating_ ? coefficients_ : trialCoefficients_;
            const auto angle = tau * (frequencies[component_] / sampleRate_) * index_;
            predicted_ += coefficients[1 + component_] * std::cos(angle)
                + coefficients[1 + partials_ + component_] * std::sin(angle);
            if (++component_ == partials_) stage_ = Stage::ErrorFinish;
            break;
        }
        case Stage::ErrorFinish:
        {
            const auto difference = samples_[index_] - predicted_;
            errorEnergy_ += difference * difference;
            maximumError_ = std::max(maximumError_, std::abs(difference));
            if (!finite(errorEnergy_)) break;
            if (++index_ == (validating_ ? samplesCount_ : trainingCount_)) stage_ = Stage::Decide;
            else stage_ = Stage::ErrorStart;
            break;
        }
        case Stage::Decide:
            if (validating_)
            {
                validationError_ = std::sqrt(errorEnergy_ / (samplesCount_ - trainingCount_)) / normalizer_;
                constantError_ = std::max(constantError_, maximumError_);
                component_ = 0; stage_ = Stage::Finalize;
            }
            else if (pass_ == Pass::Trial && errorEnergy_ >= bestEnergy_) startValidation();
            else
            {
                // At most33 coefficients and16 frequencies; bounded decision.
                for (std::size_t i = 0; i < 1 + 2 * partials_; ++i) coefficients_[i] = trialCoefficients_[i];
                for (std::size_t i = 0; i < partials_; ++i) frequencies_[i] = trialFrequencies_[i];
                bestEnergy_ = errorEnergy_; bestCondition_ = partials_ == 0 ? 1 : currentCondition_;
                constantError_ = maximumError_;
                trainingError_ = std::sqrt(bestEnergy_ / trainingCount_) / normalizer_;
                ratio_ = 0;
                for (std::size_t i = 1; i < 1 + 2 * partials_; ++i) ratio_ = std::hypot(ratio_, coefficients_[i]);
                ratio_ = ratio_ / std::sqrt(2.0) / normalizer_;
                if (pass_ == Pass::Trial) ++refinements_;
                if (partials_ != 0 && refine_ && refinements_ < maximumRefinements
                    && trainingError_ >= 1e-10 && ratio_ <= 4) startPass(Pass::Jacobian);
                else startValidation();
            }
            break;
        case Stage::Finalize:
            if (!finite(coefficients_[component_] * scale_)) break;
            if (++component_ == 1 + 2 * partials_) status_ = Status::Ready;
            break;
        }
    }
}

bool IncrementalSinusoidFit::accepted() const noexcept
{
    if (status_ != Status::Ready || !validate_) return false;
    if (partials_ == 0) return constantError_ < 1e-12 / scale_;
    return trainingError_ <= residualLimit && validationError_ <= residualLimit && ratio_ <= 4;
}
double IncrementalSinusoidFit::frequency(std::size_t partial) const noexcept
{ return status_ == Status::Ready && partial < partials_ ? frequencies_[partial] : 0; }
double IncrementalSinusoidFit::cosineCoefficient(std::size_t partial) const noexcept
{ return status_ == Status::Ready && partial < partials_ ? coefficients_[1 + partial] * scale_ : 0; }
double IncrementalSinusoidFit::sineCoefficient(std::size_t partial) const noexcept
{ return status_ == Status::Ready && partial < partials_ ? coefficients_[1 + partials_ + partial] * scale_ : 0; }
double IncrementalSinusoidFit::dc() const noexcept
{ return status_ == Status::Ready ? coefficients_[0] * scale_ : 0; }
double IncrementalSinusoidFit::trainingRelativeRms() const noexcept
{ return status_ == Status::Ready ? trainingError_ : 0; }
double IncrementalSinusoidFit::validationRelativeRms() const noexcept
{ return status_ == Status::Ready ? validationError_ : 0; }
double IncrementalSinusoidFit::componentRmsRatio() const noexcept
{ return status_ == Status::Ready ? ratio_ : 0; }
double IncrementalSinusoidFit::conditionBound() const noexcept
{ return status_ == Status::Ready ? bestCondition_ : 0; }
}
