#pragma once

#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectPartialTrackEdits.h"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace talas::audio
{
inline constexpr std::uint32_t partialTrackResynthesisVersion = 1;
inline constexpr std::uint32_t partialComplexAdapterVersion = 1;
inline constexpr std::size_t maximumRealtimePartialTrackVoices = 128;
inline constexpr std::size_t maximumOfflinePartialTrackVoices = 4096;

struct PartialTrackResynthesisProfile
{
    std::size_t maximumActiveTracks = maximumOfflinePartialTrackVoices;
    double outputGain = 1.0;
    std::uint64_t segmentId = 0;
};

struct PartialTrackResynthesisInfo
{
    std::uint32_t version = partialTrackResynthesisVersion;
    bool interpolatesAmplitudeAndFrequency = true;
    bool synthesizesBandwidthNoise = true;
    bool resetsPhaseOnDiscontinuity = true;
};

// Bandwidth uses a stateless SplitMix64-derived noise sample keyed by exact
// (segmentId, trackId, sourceFrame). The clamped bandwidth/frequency ratio
// crossfades that seeded noise with the phase-continuous sinusoid, so callback
// partitioning cannot change the rendered samples.

enum class PartialTrackResynthesisStatus : std::uint8_t
{
    Ok,
    InvalidPreparation,
    InvalidRenderSpan,
    FrameOverflow,
};

class PreparedPartialTrackResynthesizer
{
    struct TrackContinuation
    {
        std::size_t cursor = 0;
        double phase = 0.0;
    };
    struct PreparationIdentity {};

public:
    class ProcessingCheckpoint
    {
    public:
        ProcessingCheckpoint() = default;
        ProcessingCheckpoint(const ProcessingCheckpoint&) = delete;
        ProcessingCheckpoint& operator=(const ProcessingCheckpoint&) = delete;

    private:
        friend class PreparedPartialTrackResynthesizer;
        std::shared_ptr<const PreparationIdentity> preparationIdentity_;
        std::vector<TrackContinuation> activeTracks_;
        std::size_t currentSegmentIndex_ = 0;
        std::uint64_t nextSourceFrame_ = 0;
        bool initialized_ = false;
    };

    PreparedPartialTrackResynthesizer(
        const talas::project::PartialTrackAsset& asset,
        std::span<const talas::project::PartialTrackPointInterpolation>
            interpolations = {},
        PartialTrackResynthesisProfile profile = {});

    PreparedPartialTrackResynthesizer& operator=(
        const PreparedPartialTrackResynthesizer&) = delete;
    PreparedPartialTrackResynthesizer(
        PreparedPartialTrackResynthesizer&&) noexcept = default;
    PreparedPartialTrackResynthesizer& operator=(
        PreparedPartialTrackResynthesizer&&) noexcept = default;

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] std::uint64_t sourceFrameCount() const noexcept;
    [[nodiscard]] std::size_t trackCount() const noexcept;

    [[nodiscard]] PartialTrackResynthesisStatus render(
        std::uint64_t sourceStartFrame,
        std::span<double> output) noexcept;
    void reset() noexcept;
    // Allocating control-thread snapshot. All prepared points and continuation
    // state are owned; the existing noexcept move assignment restores the snapshot.
    [[nodiscard]] std::unique_ptr<PreparedPartialTrackResynthesizer> cloneProcessingState() const;
    // Control-thread preparation; storage depends on the largest active segment,
    // not the number of segments or immutable points. Explicit owned clones and
    // moves retain this preparation identity; independent preparations do not.
    [[nodiscard]] std::unique_ptr<ProcessingCheckpoint> createProcessingCheckpoint() const;
    [[nodiscard]] bool captureProcessingState(ProcessingCheckpoint& checkpoint) const noexcept;
    [[nodiscard]] bool restoreProcessingState(const ProcessingCheckpoint& checkpoint) noexcept;

private:
    PreparedPartialTrackResynthesizer(const PreparedPartialTrackResynthesizer&) = default;
    struct PreparedPoint
    {
        std::uint64_t frame = 0;
        double amplitude = 0.0;
        double frequencyHz = 0.0;
        double bandwidthHz = 0.0;
        talas::project::PartialTrackPointInterpolation interpolation =
            talas::project::PartialTrackPointInterpolation::Linear;
    };

    struct PreparedTrack
    {
        std::uint64_t trackId = 0;
        std::vector<PreparedPoint> points;
    };

    struct PreparedSegment
    {
        std::uint64_t segmentId = 0;
        std::uint64_t startFrame = 0;
        std::uint64_t endFrame = 0;
        std::vector<PreparedTrack> tracks;
    };

    bool ready_ = false;
    double sampleRate_ = 0.0;
    std::uint64_t sourceFrameCount_ = 0;
    // The file's loop markers, between which the
    // resynthesis loops; none when the end is not after the start.
    std::uint64_t loopStart_ = 0;
    std::uint64_t loopEnd_ = 0;
    double outputGain_ = 1.0;
    std::vector<PreparedSegment> segments_;
    std::shared_ptr<const PreparationIdentity> preparationIdentity_;
    std::vector<TrackContinuation> activeTracks_;
    std::size_t totalTrackCount_ = 0;
    std::size_t currentSegmentIndex_ = 0;
    std::uint64_t nextSourceFrame_ = 0;
    bool initialized_ = false;
};

struct PartialTrackOfflineRenderResult
{
    PartialTrackResynthesisStatus status =
        PartialTrackResynthesisStatus::InvalidPreparation;
    std::vector<double> samples;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == PartialTrackResynthesisStatus::Ok;
    }
};

[[nodiscard]] constexpr PartialTrackResynthesisInfo
partialTrackResynthesisInfo() noexcept
{
    return {};
}

[[nodiscard]] PartialTrackOfflineRenderResult
renderPartialTrackAssetOffline(
    const talas::project::PartialTrackAsset& asset,
    std::span<const talas::project::PartialTrackPointInterpolation>
        interpolations = {},
    PartialTrackResynthesisProfile profile = {},
    std::size_t blockFrames = 4096) noexcept;

struct PartialComplexAdapterProfile
{
    std::size_t frameSize = 1024;
    std::size_t hopSize = 256;
    double sampleRate = 48000.0;
    std::size_t maximumPartials = 256;
};

enum class PartialComplexConversionStatus : std::uint8_t
{
    Ok,
    InvalidProfile,
    InvalidInput,
    DestinationTooSmall,
};

struct PartialComplexConversionInfo
{
    std::uint32_t version = partialComplexAdapterVersion;
    std::size_t latencyFrames = 0;
    bool losesTrackIdentity = true;
    bool quantizesFrequencyToBins = true;
    bool discardsPhase = true;
};

[[nodiscard]] PartialComplexConversionInfo partialComplexConversionInfo(
    const PartialComplexAdapterProfile& profile) noexcept;

[[nodiscard]] PartialComplexConversionStatus partialsToComplexBins(
    std::span<const talas::project::PartialTrackPoint> partials,
    const PartialComplexAdapterProfile& profile,
    std::span<std::complex<double>> bins) noexcept;

[[nodiscard]] PartialComplexConversionStatus complexBinsToPartials(
    std::span<const std::complex<double>> bins,
    const PartialComplexAdapterProfile& profile,
    std::span<talas::project::PartialTrackPoint> partials,
    std::size_t& writtenPartials) noexcept;
}
