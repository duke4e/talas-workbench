#include "talas/dsp/IncrementalStationaryModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <tuple>

namespace talas::dsp
{
namespace { constexpr auto tau = 2 * std::numbers::pi; }

bool IncrementalStationaryModel::begin(std::span<const double> samples, double sampleRate) noexcept
{
    reset();
    if (samples.size() < 1024 || samples.size() > maximumSamples || (samples.size() & (samples.size() - 1)) != 0
        || !std::isfinite(sampleRate) || sampleRate <= 0)
    { status_ = Status::InvalidInput; return false; }
    std::memcpy(samples_.data(), samples.data(), samples.size_bytes());
    count_ = samples.size(); quarter_ = count_ / 4; training_ = 3 * quarter_; stride_ = count_ / 256;
    rate_ = sampleRate; status_ = Status::Running;
    return true;
}

void IncrementalStationaryModel::reset() noexcept
{
    status_ = Status::Idle; stage_ = Stage::Validate;
    count_ = training_ = quarter_ = stride_ = index_ = lane_ = guides_ = compacted_ = 0;
    order_ = 2; positiveCount_ = negativeCount_ = component_ = candidate_ = 0;
    assignments_ = assignment_ = attemptedOrders_ = attemptedAssignments_ = skippedAliasOrders_ = 0; consumedWork_ = 0;
    guidesComplete_ = haveBest_ = constant_ = false;
    rate_ = scale_ = mean_ = minimum_ = maximum_ = previous_ = left_ = middle_ = maximumPeak_ = nearest_ = nearestDistance_ = 0;
    subspace_.reset(); modes_.reset(); fit_.reset();
    // Result storage is intentionally untouched; status is the visibility gate.
}

void IncrementalStationaryModel::copyProcessingStateFrom(const IncrementalStationaryModel& source) noexcept
{
    if (this == &source) return;
    const auto fields = [](auto& value) {
        return std::tie(value.differences_, value.guideBins_, value.guideMagnitudes_,
            value.aliases_, value.frequencies_, value.positive_, value.negative_, value.paired_,
            value.candidates_, value.candidateCounts_, value.fftCursor_, value.best_, value.result_,
            value.status_, value.stage_, value.count_, value.training_, value.quarter_, value.stride_,
            value.index_, value.lane_, value.guides_, value.compacted_, value.order_,
            value.positiveCount_, value.negativeCount_, value.component_, value.candidate_,
            value.assignments_, value.assignment_, value.attemptedOrders_, value.attemptedAssignments_,
            value.skippedAliasOrders_, value.consumedWork_, value.guidesComplete_, value.haveBest_, value.constant_,
            value.rate_, value.scale_, value.mean_, value.minimum_, value.maximum_, value.previous_,
            value.left_, value.middle_, value.maximumPeak_, value.nearest_, value.nearestDistance_);
    };
    fields(*this) = fields(source);
    subspace_.reset(); modes_.reset(); fit_.reset();
    if (status_ != Status::Running) return;
    // Earlier final fits can reject and retry a higher order. The constant
    // bypass and final available order cannot retry: their fitter already owns
    // the full input, and failure rejects before either parent buffer is read.
    const auto lastAttempt = stage_ == Stage::FinalFit
        && (constant_ || order_ + 2 > 2 * maximumPartials || order_ + 2 > source.subspace_.rank());
    if (!lastAttempt) std::copy_n(source.samples_.begin(), count_, samples_.begin());
    // FFT scratch is dead after its guide scan, including on retries.
    if (stage_ == Stage::FillSpectrum || stage_ == Stage::Transform || stage_ == Stage::Spectrum)
        std::copy_n(source.spectrum_.begin(), 3 * quarter_, spectrum_.begin());
    // All transformed lanes remain live until Spectrum completes. Keep this
    // copy conservative even while FillSpectrum has only written a prefix.
    if (stage_ >= Stage::Subspace && !constant_ && !lastAttempt) subspace_ = source.subspace_;
    if (stage_ == Stage::Modes || stage_ == Stage::Roots) modes_ = source.modes_;
    if (stage_ == Stage::Fit || stage_ == Stage::FinalFit) fit_ = source.fit_;
}

std::uint64_t IncrementalStationaryModel::maximumWork() const noexcept
{
    if (count_ == 0) return 0;
    // Fixed97x95 forward/backward shape and maximum-mode solve.
    constexpr std::uint64_t r = 97, p = 2 * maximumPartials;
    constexpr auto subspace = IncrementalForwardBackwardSubspace::maximumWorkForShape();
    constexpr auto modes = r * p * p + 4 * p * p * p + 8 * p * p + 8 * p
        + IncrementalComplexEigenvalues::maximumWorkForSize(p);
    return 64ULL * count_ + 8192 + subspace + maximumPartials * (modes
        + (maximumAssignments + 1ULL) * IncrementalSinusoidFit::maximumWorkForSize(count_, maximumPartials));
}

bool IncrementalStationaryModel::finite(double value) noexcept
{
    if (std::isfinite(value)) return true;
    status_ = Status::NumericFailure; return false;
}

void IncrementalStationaryModel::nextOrder() noexcept
{ order_ += 2; stage_ = Stage::Order; }

void IncrementalStationaryModel::captureFit(Model& model) noexcept
{
    model.size = fit_.size(); model.dc = fit_.dc();
    model.training = fit_.trainingRelativeRms(); model.validation = fit_.validationRelativeRms();
    model.ratio = fit_.componentRmsRatio();
    for (std::size_t i = 0; i < model.size; ++i)
    { model.frequencies[i] = fit_.frequency(i); model.cosine[i] = fit_.cosineCoefficient(i); model.sine[i] = fit_.sineCoefficient(i); }
}

void IncrementalStationaryModel::peak(std::size_t bin, double magnitude) noexcept
{
    maximumPeak_ = std::max(maximumPeak_, magnitude);
    std::size_t position = guides_;
    if (guides_ < maximumGuides) ++guides_;
    else
    {
        position = 0;
        for (std::size_t i = 1; i < guides_; ++i)
            if (guideMagnitudes_[i] < guideMagnitudes_[position]) position = i;
        if (magnitude <= guideMagnitudes_[position]) return;
    }
    guideBins_[position] = static_cast<double>(bin); guideMagnitudes_[position] = magnitude;
}

void IncrementalStationaryModel::advance(const Radix2FftPlan& fft, std::size_t& work) noexcept
{
    if (status_ != Status::Running || work == 0) return;
    if (fft.size() != quarter_) { status_ = Status::InvalidInput; return; }
    while (work != 0 && status_ == Status::Running)
    {
        if (stage_ == Stage::Transform)
        {
            const auto before = work;
            const auto ok = fft.advanceTransform(std::span{spectrum_}.subspan(lane_ * quarter_, quarter_), fftCursor_, work);
            consumedWork_ += before - work;
            if (!ok) { status_ = Status::InvalidInput; break; }
            if (fftCursor_.complete())
            {
                if (++lane_ == 3) { index_ = 0; stage_ = Stage::Spectrum; }
                else fftCursor_ = fft.beginTransform(false);
            }
            continue;
        }
        if (stage_ == Stage::Subspace || stage_ == Stage::Modes || stage_ == Stage::Fit || stage_ == Stage::FinalFit)
        {
            const auto before = work;
            if (stage_ == Stage::Subspace)
            {
                subspace_.advance(work);
                if (subspace_.status() != IncrementalForwardBackwardSubspace::Status::Running)
                {
                    if (subspace_.status() == IncrementalForwardBackwardSubspace::Status::Ready) stage_ = Stage::Order;
                    else status_ = Status::NumericFailure;
                }
            }
            else if (stage_ == Stage::Modes)
            {
                modes_.advance(work);
                if (modes_.status() != IncrementalSubspaceModes::Status::Running)
                {
                    if (modes_.status() == IncrementalSubspaceModes::Status::Ready)
                    { index_ = positiveCount_ = negativeCount_ = 0; stage_ = Stage::Roots; }
                    else nextOrder();
                }
            }
            else
            {
                fit_.advance(work);
                if (fit_.status() != IncrementalSinusoidFit::Status::Running)
                {
                    if (stage_ == Stage::FinalFit)
                    {
                        if (fit_.accepted()) { captureFit(result_); status_ = Status::Ready; }
                        else if (constant_) status_ = Status::Rejected;
                        else nextOrder();
                    }
                    else
                    {
                        if (fit_.status() == IncrementalSinusoidFit::Status::Ready
                            && (!haveBest_ || fit_.trainingRelativeRms() < best_.training))
                        { captureFit(best_); haveBest_ = true; }
                        ++assignment_; stage_ = Stage::Assignment;
                    }
                }
            }
            consumedWork_ += before - work;
            continue;
        }
        --work; ++consumedWork_;
        switch (stage_)
        {
        case Stage::Validate:
            if (!std::isfinite(samples_[index_])) { status_ = Status::InvalidInput; break; }
            if (index_ < training_) scale_ = std::max(scale_, std::abs(samples_[index_]));
            if (++index_ == count_)
            { if (scale_ == 0) scale_ = 1; index_ = 0; stage_ = Stage::Statistics; }
            break;
        case Stage::Statistics:
        {
            const auto value = samples_[index_] / scale_;
            if (!finite(value)) break;
            if (samples_[index_] != 0 && value == 0) { status_ = Status::NumericFailure; break; }
            if (index_ == 0) minimum_ = maximum_ = value;
            minimum_ = std::min(minimum_, value); maximum_ = std::max(maximum_, value);
            mean_ += (value - mean_) / (index_ + 1);
            if (index_ % stride_ == 0)
            {
                if (index_ != 0) differences_[index_ / stride_ - 1] = value - previous_;
                previous_ = value;
            }
            if (++index_ == training_)
            {
                if (std::max(mean_ - minimum_, maximum_ - mean_) < 1e-12 / scale_)
                {
                    constant_ = true;
                    if (!fit_.begin(std::span{samples_}.first(count_), rate_, {})) status_ = Status::NumericFailure;
                    else stage_ = Stage::FinalFit;
                }
                else { index_ = 0; stage_ = Stage::FillSpectrum; }
            }
            break;
        }
        case Stage::FillSpectrum:
            spectrum_[(index_ % 3) * quarter_ + index_ / 3] = (samples_[index_] / scale_)
                * (.5 - .5 * std::cos(tau * static_cast<double>(index_) / training_));
            if (++index_ == training_) { lane_ = 0; fftCursor_ = fft.beginTransform(false); stage_ = Stage::Transform; }
            break;
        case Stage::Spectrum:
        {
            const auto phase = -tau * static_cast<double>(index_) / training_;
            const auto c = std::cos(phase), s = std::sin(phase);
            const auto bin = index_ % quarter_;
            const auto ar = spectrum_[bin].real(), ai = spectrum_[bin].imag();
            const auto br = spectrum_[quarter_ + bin].real(), bi = spectrum_[quarter_ + bin].imag();
            const auto cr = spectrum_[2 * quarter_ + bin].real(), ci = spectrum_[2 * quarter_ + bin].imag();
            const auto c2 = c * c - s * s, s2 = c * s + s * c;
            // Reconstruct the same three FFT lanes with scalar operations.
            // Normalized training bounds each lane; general complex multiply
            // overflow recovery is unnecessary and costly in Debug callbacks.
            const auto real = (ar + (c * br - s * bi)) + (c2 * cr - s2 * ci);
            const auto imag = (ai + (c * bi + s * br)) + (c2 * ci + s2 * cr);
            const auto magnitude = std::hypot(real, imag);
            if (!finite(magnitude)) break;
            // Training is normalized to |sample|<=1. Ignore FFT roundoff on
            // flat spectra (notably an impulse) and keep the first plateau edge.
            const auto tolerance = 64 * std::numeric_limits<double>::epsilon() * training_;
            if (index_ >= 2 && middle_ > left_ + tolerance && middle_ >= magnitude - tolerance)
                peak(index_ - 1, middle_);
            left_ = middle_; middle_ = magnitude;
            if (++index_ > training_ / 2) { index_ = compacted_ = 0; stage_ = Stage::CompactGuides; }
            break;
        }
        case Stage::CompactGuides:
            if (index_ < guides_)
            {
                if (guideMagnitudes_[index_] >= .01 * maximumPeak_)
                { guideBins_[compacted_] = guideBins_[index_]; guideMagnitudes_[compacted_++] = guideMagnitudes_[index_]; }
                ++index_;
            }
            else
            {
                guides_ = compacted_; guidesComplete_ = true;
                if (guides_ == 0) { status_ = Status::Rejected; break; }
                if (!subspace_.begin(differences_)) status_ = Status::NumericFailure;
                else stage_ = Stage::Subspace;
            }
            break;
        case Stage::Order:
        {
            if (order_ > 2 * maximumPartials || order_ > subspace_.rank()) { status_ = Status::Rejected; break; }
            ++attemptedOrders_;
            if (subspace_.singularValue(0) / subspace_.singularValue(order_ - 1) > 1e12) { nextOrder(); break; }
            // U0 drops one row of an orthonormal basis: sigma_min(U0)^2
            // is1 minus that row's leverage. Reject an unidentifiable prefix.
            double leverage = 0;
            for (std::size_t i = 0; i < order_; ++i)
            { const auto value = subspace_.basis(i, subspace_.rows() - 1); leverage += value * value; }
            if (leverage >= 1 || 1 / std::sqrt(1 - leverage) > 1e12) { nextOrder(); break; }
            if (!modes_.begin(subspace_, order_)) nextOrder();
            else stage_ = Stage::Modes;
            break;
        }
        case Stage::Roots:
        {
            const auto root = modes_.eigenvalue(index_);
            if (std::abs(std::abs(root) - 1) > 1e-4) { nextOrder(); break; }
            if (root.imag() > 1e-8 && positiveCount_ < maximumPartials) positive_[positiveCount_++] = root;
            else if (root.imag() < -1e-8 && negativeCount_ < maximumPartials) negative_[negativeCount_++] = root;
            else { nextOrder(); break; }
            if (++index_ == order_)
            {
                if (positiveCount_ != order_ / 2 || negativeCount_ != positiveCount_) { nextOrder(); break; }
                paired_.fill(false); component_ = 0; stage_ = Stage::Conjugates;
            }
            break;
        }
        case Stage::Conjugates:
        {
            auto found = negativeCount_; auto error = std::numeric_limits<double>::infinity();
            for (std::size_t i = 0; i < negativeCount_; ++i)
                if (!paired_[i] && std::abs(positive_[component_] - std::conj(negative_[i])) < error)
                { found = i; error = std::abs(positive_[component_] - std::conj(negative_[i])); }
            if (found == negativeCount_ || error > 1e-8) { nextOrder(); break; }
            paired_[found] = true;
            aliases_[component_] = std::arg(positive_[component_]) / tau * (rate_ / stride_);
            if (++component_ == positiveCount_)
            {
                component_ = candidate_ = 0; assignments_ = 1;
                candidateCounts_[0] = 0; nearestDistance_ = std::numeric_limits<double>::infinity(); stage_ = Stage::Candidates;
            }
            break;
        }
        case Stage::Candidates:
        {
            const auto terms = stride_ / 2 + 1;
            const auto frequency = (candidate_ % terms) * (rate_ / stride_)
                + (candidate_ < terms ? aliases_[component_] : -aliases_[component_]);
            if (frequency > 0 && frequency < rate_ / 2)
            {
                auto distance = std::numeric_limits<double>::infinity();
                for (std::size_t i = 0; i < guides_; ++i)
                    distance = std::min(distance, std::abs(frequency - guideBins_[i] * (rate_ / training_)));
                if (distance < nearestDistance_) { nearestDistance_ = distance; nearest_ = frequency; }
                if (distance <= 2 * (rate_ / training_))
                {
                    if (candidateCounts_[component_] == maximumAssignments) { status_ = Status::NumericFailure; break; }
                    candidates_[component_][candidateCounts_[component_]++] = frequency;
                }
            }
            if (++candidate_ == 2 * terms) stage_ = Stage::NextCandidates;
            break;
        }
        case Stage::NextCandidates:
        {
            auto& count = candidateCounts_[component_];
            if (!std::isfinite(nearestDistance_)) { nextOrder(); break; }
            bool present = false;
            for (std::size_t i = 0; i < count; ++i) present |= candidates_[component_][i] == nearest_;
            if (!present)
            {
                if (count == maximumAssignments) { status_ = Status::NumericFailure; break; }
                candidates_[component_][count++] = nearest_;
            }
            if (count == 0 || assignments_ > maximumAssignments / count) { ++skippedAliasOrders_; nextOrder(); break; }
            assignments_ *= count;
            if (++component_ == positiveCount_)
            { assignment_ = 0; haveBest_ = false; stage_ = Stage::Assignment; }
            else
            { candidate_ = 0; candidateCounts_[component_] = 0; nearestDistance_ = std::numeric_limits<double>::infinity(); stage_ = Stage::Candidates; }
            break;
        }
        case Stage::Assignment:
            if (assignment_ == assignments_)
            {
                if (!haveBest_ || best_.ratio > 4) { nextOrder(); break; }
                if (!fit_.begin(std::span{samples_}.first(count_), rate_, std::span{best_.frequencies}.first(best_.size), true, true))
                    status_ = Status::NumericFailure;
                else stage_ = Stage::FinalFit;
            }
            else
            {
                auto selection = assignment_;
                for (std::size_t i = positiveCount_; i-- > 0;)
                { frequencies_[i] = candidates_[i][selection % candidateCounts_[i]]; selection /= candidateCounts_[i]; }
                std::sort(frequencies_.begin(), frequencies_.begin() + static_cast<std::ptrdiff_t>(positiveCount_));
                bool duplicate = false;
                for (std::size_t i = 1; i < positiveCount_; ++i) duplicate |= frequencies_[i] - frequencies_[i - 1] < 1e-6;
                if (duplicate) { ++assignment_; break; }
                ++attemptedAssignments_;
                // A unique training-derived assignment needs no ranking pass.
                // Refine and validate that same candidate directly; held-out
                // samples still never choose among frequency assignments.
                const auto unique = assignments_ == 1;
                if (!fit_.begin(std::span{samples_}.first(count_), rate_,
                    std::span{frequencies_}.first(positiveCount_), unique, unique))
                    status_ = Status::NumericFailure;
                else stage_ = unique ? Stage::FinalFit : Stage::Fit;
            }
            break;
        case Stage::Transform: case Stage::Subspace: case Stage::Modes: case Stage::Fit: case Stage::FinalFit: break;
        }
    }
}

double IncrementalStationaryModel::frequency(std::size_t i) const noexcept
{ return status_ == Status::Ready && i < result_.size ? result_.frequencies[i] : 0; }
double IncrementalStationaryModel::cosineCoefficient(std::size_t i) const noexcept
{ return status_ == Status::Ready && i < result_.size ? result_.cosine[i] : 0; }
double IncrementalStationaryModel::sineCoefficient(std::size_t i) const noexcept
{ return status_ == Status::Ready && i < result_.size ? result_.sine[i] : 0; }
double IncrementalStationaryModel::dc() const noexcept { return status_ == Status::Ready ? result_.dc : 0; }
double IncrementalStationaryModel::trainingRelativeRms() const noexcept { return status_ == Status::Ready ? result_.training : 0; }
double IncrementalStationaryModel::validationRelativeRms() const noexcept { return status_ == Status::Ready ? result_.validation : 0; }
double IncrementalStationaryModel::guideFrequency(std::size_t i) const noexcept
{ return guidesComplete_ && i < guides_ ? guideBins_[i] * (rate_ / training_) : 0; }
double IncrementalStationaryModel::guideMagnitude(std::size_t i) const noexcept
{ return guidesComplete_ && i < guides_ ? guideMagnitudes_[i] : 0; }
}
