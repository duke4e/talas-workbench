#include "talas/runtime/AnalysisTestInputSource.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace talas::runtime
{
bool AnalysisTestInputSource::prepare(
    AnalysisTestInputPreparation preparation,
    AnalysisTestInputSource& destination,
    std::string& diagnostic)
{
    diagnostic.clear();
    if (!talas::project::isKnownWorkspaceAnalysisTestSourceKind(
            preparation.kind))
    {
        diagnostic = "Analysis test input source kind is unknown";
        return false;
    }
    if (!std::isfinite(preparation.sampleRate)
        || preparation.sampleRate <= 0.0)
    {
        diagnostic = "Analysis test input requires a finite positive sample rate";
        return false;
    }
    if (preparation.kind
            == talas::project::WorkspaceAnalysisTestSourceKind::Sine
        && (!std::isfinite(preparation.sineFrequencyHz)
            || preparation.sineFrequencyHz
                < talas::project::minimumWorkspaceAnalysisSineFrequencyHz
            || preparation.sineFrequencyHz
                > talas::project::maximumWorkspaceAnalysisSineFrequencyHz
            || preparation.sineFrequencyHz >= preparation.sampleRate * 0.5))
    {
        diagnostic = "Analysis sine frequency is outside the prepared Nyquist range";
        return false;
    }
    if (preparation.frozenMonoSamples.size()
        > maximumAnalysisFrozenAssetFrames)
    {
        diagnostic = "Frozen Analysis asset exceeds the bounded frame limit";
        return false;
    }
    if (preparation.kind
            == talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset
        && preparation.frozenMonoSamples.empty())
    {
        diagnostic = "Frozen Analysis asset contains no mono samples";
        return false;
    }
    if (preparation.kind
            != talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset
        && !preparation.frozenMonoSamples.empty())
    {
        diagnostic = "Non-asset Analysis source carries contradictory samples";
        return false;
    }
    if (!std::ranges::all_of(
            preparation.frozenMonoSamples,
            [](float sample) { return std::isfinite(sample); }))
    {
        diagnostic = "Frozen Analysis asset contains a non-finite sample";
        return false;
    }

    destination = {};
    destination.kind_ = preparation.kind;
    destination.sampleRate_ = preparation.sampleRate;
    destination.sineFrequencyHz_ = preparation.sineFrequencyHz;
    destination.frozenMonoSamples_ =
        std::move(preparation.frozenMonoSamples);
    destination.loopFrozenAsset_ = preparation.loopFrozenAsset;
    destination.reset();
    return true;
}

void AnalysisTestInputSource::reset() noexcept
{
    sinePhase_ = 0.0;
    frozenCursor_ = 0;
    impulsePending_ = kind_
        == talas::project::WorkspaceAnalysisTestSourceKind::Impulse;
    exhausted_ = false;
}

void AnalysisTestInputSource::render(std::span<float> destination) noexcept
{
    switch (kind_)
    {
    case talas::project::WorkspaceAnalysisTestSourceKind::None:
    case talas::project::WorkspaceAnalysisTestSourceKind::Silence:
        std::ranges::fill(destination, 0.0F);
        return;
    case talas::project::WorkspaceAnalysisTestSourceKind::Impulse:
        std::ranges::fill(destination, 0.0F);
        if (impulsePending_ && !destination.empty())
        {
            destination.front() = 1.0F;
            impulsePending_ = false;
            exhausted_ = true;
        }
        return;
    case talas::project::WorkspaceAnalysisTestSourceKind::Sine:
    {
        const auto increment = 2.0 * std::numbers::pi
            * sineFrequencyHz_ / sampleRate_;
        for (auto& sample : destination)
        {
            sample = static_cast<float>(std::sin(sinePhase_));
            sinePhase_ += increment;
            if (sinePhase_ >= 2.0 * std::numbers::pi)
                sinePhase_ -= 2.0 * std::numbers::pi;
        }
        return;
    }
    case talas::project::WorkspaceAnalysisTestSourceKind::FrozenAsset:
        for (auto& sample : destination)
        {
            if (frozenCursor_ >= frozenMonoSamples_.size())
            {
                if (loopFrozenAsset_)
                    frozenCursor_ = 0;
                else
                {
                    sample = 0.0F;
                    exhausted_ = true;
                    continue;
                }
            }
            sample = frozenMonoSamples_[frozenCursor_++];
        }
        return;
    }
}

talas::project::WorkspaceAnalysisTestSourceKind
AnalysisTestInputSource::kind() const noexcept
{
    return kind_;
}

std::size_t AnalysisTestInputSource::frozenFrameCount() const noexcept
{
    return frozenMonoSamples_.size();
}

bool AnalysisTestInputSource::exhausted() const noexcept
{
    return exhausted_;
}
}
