#pragma once

#include "talas/project/WorkspaceSessionState.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace talas::runtime
{
inline constexpr std::size_t maximumAnalysisFrozenAssetFrames = 4'194'304;

struct AnalysisTestInputPreparation
{
    talas::project::WorkspaceAnalysisTestSourceKind kind =
        talas::project::WorkspaceAnalysisTestSourceKind::None;
    double sampleRate = 0.0;
    double sineFrequencyHz = 440.0;
    std::vector<float> frozenMonoSamples;
    bool loopFrozenAsset = false;
};

class AnalysisTestInputSource
{
public:
    [[nodiscard]] static bool prepare(
        AnalysisTestInputPreparation preparation,
        AnalysisTestInputSource& destination,
        std::string& diagnostic);

    void reset() noexcept;
    void render(std::span<float> destination) noexcept;

    [[nodiscard]] talas::project::WorkspaceAnalysisTestSourceKind kind() const noexcept;
    [[nodiscard]] std::size_t frozenFrameCount() const noexcept;
    [[nodiscard]] bool exhausted() const noexcept;

private:
    talas::project::WorkspaceAnalysisTestSourceKind kind_ =
        talas::project::WorkspaceAnalysisTestSourceKind::None;
    double sampleRate_ = 0.0;
    double sineFrequencyHz_ = 440.0;
    double sinePhase_ = 0.0;
    std::vector<float> frozenMonoSamples_;
    std::size_t frozenCursor_ = 0;
    bool loopFrozenAsset_ = false;
    bool impulsePending_ = false;
    bool exhausted_ = false;
};
}
