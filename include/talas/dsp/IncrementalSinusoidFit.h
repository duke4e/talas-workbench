#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace talas::dsp
{
// Research primitive: fit/refine supplied original-rate candidate frequencies.
// Frequency discovery, alias choice and audio integration are separate work.
class IncrementalSinusoidFit
{
public:
    static constexpr std::size_t maximumSamples = 65536, maximumPartials = 16;
    static constexpr std::size_t maximumColumns = 1 + 3 * maximumPartials;
    static constexpr std::size_t maximumRefinements = 3;
    static constexpr double maximumConditionBound = 1e12, residualLimit = 1e-4;
    enum class Status { Idle, Running, Ready, InvalidInput, IllConditioned, NumericOverflow, DynamicRangeLoss };

    // Copies samples immediately, at most65536. Sample validation/normalization
    // are staged; true means the shape/controls and owned capture are valid,
    // not that sample values or a model have passed. N>=32, divisible by4;
    // first3N/4 train, finalN/4 validate. Candidates strictly increase, are
    // separated by >=1e-6Hz, and lie strictly inside (0, sampleRate/2).
    // Empty candidates request a constant model. No caller memory is retained.
    [[nodiscard]] bool begin(std::span<const double> samples, double sampleRate,
                             std::span<const double> frequencies, bool refine = true, bool validate = true) noexcept;
    // With validate=false only the training prefix is inspected. Such a fit
    // can rank aliases, but accepted() is always false until a validated run.
    // One unit is sample validation/normalization/statistics, one sinusoid/derivative, a QR matrix-cell
    // rotation, triangular-solve product/division, or bounded stage decision.
    // Zero work is inert. This is not a CPU-time bound; capture is immediate.
    void advance(std::size_t& work) noexcept;
    void reset() noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] bool accepted() const noexcept;
    [[nodiscard]] bool validationPerformed() const noexcept { return status_ == Status::Ready && validate_; }
    [[nodiscard]] std::size_t size() const noexcept { return partials_; }
    [[nodiscard]] std::size_t refinements() const noexcept { return refinements_; }
    [[nodiscard]] std::size_t consumedWork() const noexcept { return consumedWork_; }
    [[nodiscard]] std::size_t maximumWork() const noexcept;
    [[nodiscard]] static constexpr std::size_t maximumWorkForSize(std::size_t n, std::size_t partials) noexcept
    {
        if (n < 32 || n > maximumSamples || n % 4 != 0 || partials > maximumPartials) return 0;
        const auto train = 3 * n / 4;
        const auto columns = 1 + 3 * partials;
        const auto triangular = columns * (columns + 5) / 2;
        // At the declared maxima this is 466,971,271, and every intermediate
        // also fits in a 32-bit size_t. The bounds above keep these products safe.
        return 2 * n + 7 * (train * (partials + 4 + triangular)
            + columns * columns * columns + 6 * columns * columns + 10 * columns + 64)
            + (4 * train + n - train) * (partials + 2) + 128;
    }
    [[nodiscard]] double frequency(std::size_t partial) const noexcept;
    [[nodiscard]] double cosineCoefficient(std::size_t partial) const noexcept;
    [[nodiscard]] double sineCoefficient(std::size_t partial) const noexcept;
    [[nodiscard]] double dc() const noexcept;
    [[nodiscard]] double trainingRelativeRms() const noexcept;
    [[nodiscard]] double validationRelativeRms() const noexcept;
    [[nodiscard]] double componentRmsRatio() const noexcept;
    [[nodiscard]] double conditionBound() const noexcept;

private:
    enum class Pass { Initial, Jacobian, Trial };
    enum class Stage { Validate, NormalizeStatistics, Clear, RowStart, Basis, Rotation, Rotate,
        ConditionNorm, InverseStart, InverseSum, InverseDivide, SolveStart,
        SolveSum, SolveDivide, Store, StepSize, TrialFrequency,
        ErrorStart, ErrorBasis, ErrorFinish, Decide, Finalize };
    void startPass(Pass pass) noexcept;
    void rejectSolve() noexcept;
    void startValidation() noexcept;
    void startError(bool validation) noexcept;
    void nextRotation() noexcept;
    [[nodiscard]] bool finite(double value) noexcept;
    static constexpr std::size_t offset(std::size_t row, std::size_t column) noexcept
    { return row * (maximumColumns + 1) + column; }

    std::array<double, maximumSamples> samples_{};
    std::array<double, maximumPartials> initialFrequencies_{}, frequencies_{}, trialFrequencies_{};
    std::array<double, 1 + 2 * maximumPartials> coefficients_{}, trialCoefficients_{};
    std::array<double, maximumColumns * (maximumColumns + 1)> qr_{};
    std::array<double, maximumColumns + 1> rowValues_{};
    std::array<double, maximumColumns> solution_{}, inverseColumn_{};
    Status status_ = Status::Idle;
    Stage stage_ = Stage::Validate;
    Pass pass_ = Pass::Initial;
    std::size_t samplesCount_ = 0, trainingCount_ = 0, partials_ = 0, columns_ = 0;
    std::size_t index_ = 0, component_ = 0, pivot_ = 0, column_ = 0, inverseIndex_ = 0;
    std::size_t solveRow_ = 0, refinements_ = 0, consumedWork_ = 0;
    bool refine_ = false, validating_ = false, validate_ = true;
    double sampleRate_ = 0, scale_ = 0, mean_ = 0, variance_ = 0, normalizer_ = 1;
    double cosine_ = 0, sine_ = 0, predicted_ = 0, sum_ = 0, norm_ = 0, inverseNorm_ = 0;
    double currentCondition_ = 0, bestCondition_ = 0, stepScale_ = 0;
    double errorEnergy_ = 0, maximumError_ = 0, bestEnergy_ = 0, trainingError_ = 0;
    double validationError_ = 0, ratio_ = 0, constantError_ = 0;
};
}
