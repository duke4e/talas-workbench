#pragma once

#include "talas/dsp/IncrementalSubspaceModes.h"
#include "talas/dsp/IncrementalForwardBackwardSubspace.h"
#include "talas/dsp/IncrementalSinusoidFit.h"
#include "talas/dsp/Radix2Fft.h"

#include <cstdint>

namespace talas::dsp
{
// Source-only stationary component estimator. Production phase continuity,
// confidence transitions and audio scheduling are separate caller contracts.
// Sixteen partials is a work cap.
class IncrementalStationaryModel
{
public:
    static constexpr std::size_t maximumSamples = 65536;
    static constexpr std::size_t maximumPartials = IncrementalSinusoidFit::maximumPartials;
    static_assert(2 * maximumPartials <= IncrementalSubspaceModes::maximumModes);
    static constexpr std::size_t maximumGuides = 16, maximumAssignments = 256;
    enum class Status { Idle, Running, Ready, Rejected, InvalidInput, NumericFailure };

    // Own raw samples now. Power-of-two N1024..65536, finite positive rate;
    // sample validation is staged. No frequencies or source truth are inputs.
    [[nodiscard]] bool begin(std::span<const double> samples, double sampleRate) noexcept;
    // Caller prepares an N/4 FFT plan outside processing; it is never retained.
    // A unit is a sample/FFT operation, mode/candidate decision, delegated
    // solver unit, or bounded setup (including an owned N-sample fit copy).
    void advance(const Radix2FftPlan& fft, std::size_t& work) noexcept;
    // Copy only live processing storage into an already prepared fixed-capacity
    // worker. Default copy/assignment remain complete, trivially copyable state.
    void copyProcessingStateFrom(const IncrementalStationaryModel& source) noexcept;
    void reset() noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] std::size_t size() const noexcept { return status_ == Status::Ready ? result_.size : 0; }
    [[nodiscard]] std::uint64_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] std::uint64_t maximumWork() const noexcept;
    [[nodiscard]] std::size_t attemptedOrders() const noexcept { return attemptedOrders_; }
    [[nodiscard]] std::size_t attemptedAssignments() const noexcept { return attemptedAssignments_; }
    [[nodiscard]] std::size_t skippedAliasOrders() const noexcept { return skippedAliasOrders_; }
    [[nodiscard]] double frequency(std::size_t i) const noexcept;
    [[nodiscard]] double cosineCoefficient(std::size_t i) const noexcept;
    [[nodiscard]] double sineCoefficient(std::size_t i) const noexcept;
    [[nodiscard]] double dc() const noexcept;
    [[nodiscard]] double trainingRelativeRms() const noexcept;
    [[nodiscard]] double validationRelativeRms() const noexcept;
    // Diagnostic guide data, available only after a completed spectral scan.
    [[nodiscard]] std::size_t guideCount() const noexcept { return guidesComplete_ ? guides_ : 0; }
    [[nodiscard]] double guideFrequency(std::size_t i) const noexcept;
    [[nodiscard]] double guideMagnitude(std::size_t i) const noexcept;

private:
    friend struct IncrementalStationaryModelTestAccess;
    enum class Stage { Validate, Statistics, FillSpectrum, Transform, Spectrum,
        CompactGuides, Subspace, Order, Modes, Roots, Conjugates, Candidates,
        NextCandidates, Assignment, Fit, FinalFit };
    struct Model
    {
        std::array<double, maximumPartials> frequencies{}, cosine{}, sine{};
        std::size_t size = 0;
        double dc = 0, training = 0, validation = 0, ratio = 0;
    };
    void nextOrder() noexcept;
    void captureFit(Model& model) noexcept;
    void peak(std::size_t bin, double magnitude) noexcept;
    [[nodiscard]] bool finite(double value) noexcept;

    std::array<double, maximumSamples> samples_{};
    std::array<std::complex<double>, 3 * maximumSamples / 4> spectrum_{};
    std::array<double, 191> differences_{};
    std::array<double, maximumGuides> guideBins_{}, guideMagnitudes_{};
    std::array<double, maximumPartials> aliases_{}, frequencies_{};
    std::array<std::complex<double>, maximumPartials> positive_{}, negative_{};
    std::array<bool, maximumPartials> paired_{};
    std::array<std::array<double, maximumAssignments>, maximumPartials> candidates_{};
    std::array<std::size_t, maximumPartials> candidateCounts_{};
    Radix2FftPlan::Cursor fftCursor_;
    IncrementalForwardBackwardSubspace subspace_;
    IncrementalSubspaceModes modes_;
    IncrementalSinusoidFit fit_;
    Model best_{}, result_{};
    Status status_ = Status::Idle;
    Stage stage_ = Stage::Validate;
    std::size_t count_ = 0, training_ = 0, quarter_ = 0, stride_ = 0;
    std::size_t index_ = 0, lane_ = 0, guides_ = 0, compacted_ = 0, order_ = 2;
    std::size_t positiveCount_ = 0, negativeCount_ = 0, component_ = 0, candidate_ = 0;
    std::size_t assignments_ = 0, assignment_ = 0, attemptedOrders_ = 0, attemptedAssignments_ = 0;
    std::size_t skippedAliasOrders_ = 0;
    std::uint64_t consumedWork_ = 0;
    bool guidesComplete_ = false, haveBest_ = false, constant_ = false;
    double rate_ = 0, scale_ = 0, mean_ = 0, minimum_ = 0, maximum_ = 0, previous_ = 0;
    double left_ = 0, middle_ = 0, maximumPeak_ = 0, nearest_ = 0, nearestDistance_ = 0;
};
}
