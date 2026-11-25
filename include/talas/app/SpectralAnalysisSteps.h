#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace talas::app
{
// The steps Spectral Analysis guides through: select the sample; set the
// analysis frequency, filter and level and audition them; create the spectrum
// file, or set the quasi-harmonic parameters: remove the pitched parts with
// the unpitched fader, trace the fundamental, and create the quasi-harmonic
// spectrum file, whose page has buttons back to the steps before.
enum class SpectralAnalysisStep : std::uint8_t
{
    Select,
    Parameters,
    Save,
    Unpitched,
    Trace,
    Harmonic,
};

// What a step plays, looped with the silence after it.
enum class SpectralAnalysisAudition : std::uint8_t
{
    OriginalSample,
    Resynthesis,
    UnpitchedOnly,
    FundamentalOnly,
    AllHarmonics,
};

// One second of silence follows each repetition unless another is typed.
inline constexpr double defaultSpectralAnalysisSilenceSeconds = 1.0;

class SpectralAnalysisSteps
{
public:
    [[nodiscard]] SpectralAnalysisStep step() const noexcept { return step_; }
    [[nodiscard]] bool hasSample() const noexcept { return hasSample_; }
    // A sample selected: it is heard at once.
    void sampleSelected() noexcept;
    // Next: on from Select once a sample is chosen; Set Quasi-Harmonic
    // Parameters is Next from the spectrum file's step; none after the
    // last.
    [[nodiscard]] bool canGoOn() const noexcept;
    bool next() noexcept;
    bool back() noexcept;
    // The last page's New sample, Adjust range & response, Adjust
    // pitched/unpitched and Adjust fundamental: back to an earlier step.
    bool goBackTo(SpectralAnalysisStep step) noexcept;

    // The radio buttons a step offers, and what it plays: the original
    // on Select; the resynthesis or the original once Audition is pressed;
    // what the unpitched fader leaves; the original, a sine at the
    // fundamental or all its harmonics while tracing.
    [[nodiscard]] static std::vector<SpectralAnalysisAudition> auditions(SpectralAnalysisStep step);
    [[nodiscard]] SpectralAnalysisAudition audition() const noexcept { return audition_; }
    bool chooseAudition(SpectralAnalysisAudition audition) noexcept;
    [[nodiscard]] static std::string title(SpectralAnalysisStep step);

private:
    void enter(SpectralAnalysisStep step) noexcept;

    SpectralAnalysisStep step_ = SpectralAnalysisStep::Select;
    SpectralAnalysisAudition audition_ = SpectralAnalysisAudition::OriginalSample;
    // What the parameters' and trace's radio buttons were left on.
    SpectralAnalysisAudition parametersAudition_ = SpectralAnalysisAudition::Resynthesis;
    SpectralAnalysisAudition traceAudition_ = SpectralAnalysisAudition::FundamentalOnly;
    bool hasSample_ = false;
};
}
