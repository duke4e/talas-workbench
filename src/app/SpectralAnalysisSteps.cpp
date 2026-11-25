#include "talas/app/SpectralAnalysisSteps.h"

#include <algorithm>

namespace talas::app
{
void SpectralAnalysisSteps::sampleSelected() noexcept
{
    hasSample_ = true;
    if (step_ == SpectralAnalysisStep::Select)
        audition_ = SpectralAnalysisAudition::OriginalSample;
}

bool SpectralAnalysisSteps::canGoOn() const noexcept
{
    return step_ != SpectralAnalysisStep::Harmonic && (step_ != SpectralAnalysisStep::Select || hasSample_);
}

bool SpectralAnalysisSteps::next() noexcept
{
    if (!canGoOn())
        return false;
    enter(static_cast<SpectralAnalysisStep>(static_cast<int>(step_) + 1));
    return true;
}

bool SpectralAnalysisSteps::back() noexcept
{
    if (step_ == SpectralAnalysisStep::Select)
        return false;
    enter(static_cast<SpectralAnalysisStep>(static_cast<int>(step_) - 1));
    return true;
}

bool SpectralAnalysisSteps::goBackTo(SpectralAnalysisStep step) noexcept
{
    if (static_cast<int>(step) >= static_cast<int>(step_))
        return false;
    enter(step);
    return true;
}

std::vector<SpectralAnalysisAudition> SpectralAnalysisSteps::auditions(SpectralAnalysisStep step)
{
    switch (step)
    {
    case SpectralAnalysisStep::Select:
        return {SpectralAnalysisAudition::OriginalSample};
    case SpectralAnalysisStep::Parameters:
    case SpectralAnalysisStep::Save:
        return {SpectralAnalysisAudition::Resynthesis, SpectralAnalysisAudition::OriginalSample};
    case SpectralAnalysisStep::Unpitched:
        return {SpectralAnalysisAudition::UnpitchedOnly};
    case SpectralAnalysisStep::Trace:
    case SpectralAnalysisStep::Harmonic:
        return {SpectralAnalysisAudition::OriginalSample, SpectralAnalysisAudition::FundamentalOnly,
            SpectralAnalysisAudition::AllHarmonics};
    }
    return {};
}

bool SpectralAnalysisSteps::chooseAudition(SpectralAnalysisAudition audition) noexcept
{
    const auto offered = auditions(step_);
    if (std::ranges::find(offered, audition) == offered.end())
        return false;
    audition_ = audition;
    if (step_ == SpectralAnalysisStep::Parameters || step_ == SpectralAnalysisStep::Save)
        parametersAudition_ = audition;
    else if (step_ == SpectralAnalysisStep::Trace || step_ == SpectralAnalysisStep::Harmonic)
        traceAudition_ = audition;
    return true;
}

std::string SpectralAnalysisSteps::title(SpectralAnalysisStep step)
{
    switch (step)
    {
    case SpectralAnalysisStep::Select:
        return "Select a sample";
    case SpectralAnalysisStep::Parameters:
        return "Set the analysis parameters";
    case SpectralAnalysisStep::Save:
        return "Create the spectrum file";
    case SpectralAnalysisStep::Unpitched:
        return "Remove the pitched parts";
    case SpectralAnalysisStep::Trace:
        return "Trace the fundamental";
    case SpectralAnalysisStep::Harmonic:
        return "Create the quasi-harmonic spectrum file";
    }
    return {};
}

void SpectralAnalysisSteps::enter(SpectralAnalysisStep step) noexcept
{
    step_ = step;
    switch (step)
    {
    case SpectralAnalysisStep::Select:
        audition_ = SpectralAnalysisAudition::OriginalSample;
        break;
    case SpectralAnalysisStep::Parameters:
    case SpectralAnalysisStep::Save:
        audition_ = parametersAudition_;
        break;
    case SpectralAnalysisStep::Unpitched:
        audition_ = SpectralAnalysisAudition::UnpitchedOnly;
        break;
    case SpectralAnalysisStep::Trace:
    case SpectralAnalysisStep::Harmonic:
        audition_ = traceAudition_;
        break;
    }
}
}
