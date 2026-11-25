#pragma once

#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::project
{
struct UntrackedPartialFrame
{
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    std::vector<PartialTrackPoint> peaks;
};

struct LegacyPartialTriplesFrame
{
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    std::vector<double> encodedTriples;
};

struct CapturedPartialPoint
{
    std::optional<std::uint64_t> trackId;
    double amplitude = 0.0;
    double frequencyHz = 0.0;
    double bandwidthHz = 0.0;
};

struct CapturedPartialFrame
{
    std::uint64_t stageId = 0;
    std::uint64_t bindingGeneration = 0;
    std::uint64_t sequence = 0;
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    std::vector<CapturedPartialPoint> points;
};

struct PartialTrackAnalysisOptions
{
    double maximumFrequencyDistanceHz = 40.0;
    double maximumRelativeFrequencyDistance = 0.08;
    double maximumRelativeAmplitudeDistance = 1.0;
    std::uint64_t maximumGapFrames = 4096;
};

inline constexpr std::size_t
maximumPartialTrackTrackingCandidatesPerPeak = 64;
inline constexpr std::size_t
maximumPartialTrackActiveTrackingTracks =
    maximumPartialTrackPointsPerFrame;

enum class PartialTrackAnalysisErrorCode : std::uint8_t
{
    InvalidInput,
    LimitExceeded,
    InvalidLegacyTriples,
    AnalysisFailed,
    Cancelled,
    AllocationFailed,
};

struct PartialTrackAnalysisError
{
    PartialTrackAnalysisErrorCode code =
        PartialTrackAnalysisErrorCode::InvalidInput;
    std::size_t frameIndex = 0;
    std::string message;
};

struct PartialTrackAnalysisResult
{
    PartialTrackAsset asset;
    std::optional<PartialTrackAnalysisError> error;
    std::uint64_t acceptedFrameCount = 0;
    std::uint64_t acceptedPointCount = 0;
    std::uint16_t progressPermille = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] PartialTrackAnalysisResult trackPartialSpectrumFrames(
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const UntrackedPartialFrame> frames,
    PartialTrackAnalysisOptions options = {},
    std::function<bool()> shouldCancel = {});

[[nodiscard]] PartialTrackAnalysisResult convertLegacyPartialTriples(
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const LegacyPartialTriplesFrame> frames,
    PartialTrackAnalysisOptions options = {},
    std::function<bool()> shouldCancel = {});

[[nodiscard]] PartialTrackAnalysisResult assembleCapturedPartialFrames(
    std::uint64_t expectedStageId,
    std::uint64_t expectedBindingGeneration,
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const CapturedPartialFrame> frames,
    PartialTrackAnalysisOptions options = {},
    std::function<bool()> shouldCancel = {});

}
