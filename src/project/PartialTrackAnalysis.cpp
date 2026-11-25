#include "talas/project/PartialTrackAnalysis.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] PartialTrackAnalysisResult failure(
    PartialTrackAnalysisErrorCode code,
    std::string message,
    std::size_t frameIndex = 0)
{
    PartialTrackAnalysisResult result;
    result.error = PartialTrackAnalysisError{
        code, frameIndex, std::move(message)};
    return result;
}

[[nodiscard]] bool validOptions(
    const PartialTrackAnalysisOptions& options) noexcept
{
    return std::isfinite(options.maximumFrequencyDistanceHz)
        && options.maximumFrequencyDistanceHz >= 0.0
        && std::isfinite(options.maximumRelativeFrequencyDistance)
        && options.maximumRelativeFrequencyDistance >= 0.0
        && options.maximumRelativeFrequencyDistance <= 1.0
        && std::isfinite(options.maximumRelativeAmplitudeDistance)
        && options.maximumRelativeAmplitudeDistance >= 0.0
        && options.maximumRelativeAmplitudeDistance <= 1.0
        && options.maximumGapFrames <= maximumPartialTrackSourceFrames;
}

struct ActiveTrack
{
    std::uint64_t trackId = 0;
    std::uint64_t sourceFrame = 0;
    double frequencyHz = 0.0;
    double amplitude = 0.0;
    bool used = false;
    std::uint64_t nextSourceFrame = 0;
    double nextFrequencyHz = 0.0;
    double nextAmplitude = 0.0;
};
}

PartialTrackAnalysisResult trackPartialSpectrumFrames(
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const UntrackedPartialFrame> frames,
    PartialTrackAnalysisOptions options,
    std::function<bool()> shouldCancel)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || sampleRate > maximumPartialTrackSampleRate
        || sourceFrameCount == 0
        || sourceFrameCount > maximumPartialTrackSourceFrames
        || frames.size() > maximumPartialTrackAssetFrames
        || !validOptions(options))
    {
        return failure(
            PartialTrackAnalysisErrorCode::InvalidInput,
            "Partial-track analysis input or options are invalid");
    }

    PartialTrackAnalysisResult result;
    result.asset.sampleRate = sampleRate;
    result.asset.sourceFrameCount = sourceFrameCount;
    std::size_t totalPoints = 0;
    try
    {
        result.asset.frames.reserve(frames.size());
        std::vector<ActiveTrack> active;
        active.reserve(maximumPartialTrackActiveTrackingTracks);
        std::uint64_t nextTrackId = 1;
        std::uint64_t previousSegment = 0;
        std::uint64_t previousFrame = 0;
        for (std::size_t frameIndex = 0;
             frameIndex < frames.size();
             ++frameIndex)
        {
            if (shouldCancel && shouldCancel())
            {
                return failure(
                    PartialTrackAnalysisErrorCode::Cancelled,
                    "Partial-track analysis cancelled",
                    frameIndex);
            }
            const auto& source = frames[frameIndex];
            if (source.segmentId == 0
                || source.sourceFrame >= sourceFrameCount
                || source.peaks.size()
                    > maximumPartialTrackPointsPerFrame
                || (frameIndex != 0
                    && (source.segmentId < previousSegment
                        || (source.segmentId == previousSegment
                            && source.sourceFrame <= previousFrame))))
            {
                return failure(
                    PartialTrackAnalysisErrorCode::InvalidInput,
                    "Partial-track analysis frames must be bounded and strictly ordered",
                    frameIndex);
            }
            if (source.peaks.size()
                > maximumPartialTrackAssetPoints - totalPoints)
            {
                return failure(
                    PartialTrackAnalysisErrorCode::LimitExceeded,
                    "Partial-track analysis point count exceeds the fixed limit",
                    frameIndex);
            }
            totalPoints += source.peaks.size();
            if (frameIndex == 0 || source.segmentId != previousSegment)
                active.clear();
            else
                std::erase_if(
                    active,
                    [&](const auto& track) {
                        return source.sourceFrame < track.sourceFrame
                            || source.sourceFrame - track.sourceFrame
                                > options.maximumGapFrames;
                    });
            for (auto& track : active)
            {
                track.used = false;
                track.nextSourceFrame = track.sourceFrame;
                track.nextFrequencyHz = track.frequencyHz;
                track.nextAmplitude = track.amplitude;
            }
            std::ranges::sort(
                active,
                [](const auto& left, const auto& right) {
                    if (left.frequencyHz != right.frequencyHz)
                        return left.frequencyHz < right.frequencyHz;
                    return left.trackId < right.trackId;
                });

            PartialTrackFrame output{
                source.segmentId, source.sourceFrame, {}};
            output.points.reserve(source.peaks.size());
            std::vector<ActiveTrack> newTracks;
            newTracks.reserve(source.peaks.size());
            auto peaks = source.peaks;
            std::ranges::sort(peaks, {}, &PartialTrackPoint::frequencyHz);
            for (std::size_t peakIndex = 0;
                 peakIndex < peaks.size();
                 ++peakIndex)
            {
                if ((peakIndex & 63U) == 0U
                    && shouldCancel && shouldCancel())
                {
                    return failure(
                        PartialTrackAnalysisErrorCode::Cancelled,
                        "Partial-track analysis cancelled",
                        frameIndex);
                }
                auto peak = peaks[peakIndex];
                if (peak.trackId != 0
                    || !std::isfinite(peak.amplitude)
                    || peak.amplitude < 0.0
                    || !std::isfinite(peak.frequencyHz)
                    || peak.frequencyHz <= 0.0
                    || peak.frequencyHz > sampleRate * 0.5
                    || !std::isfinite(peak.bandwidthHz)
                    || peak.bandwidthHz < 0.0)
                {
                    return failure(
                        PartialTrackAnalysisErrorCode::InvalidInput,
                        "Untracked partial peak is invalid",
                        frameIndex);
                }
                ActiveTrack* match = nullptr;
                double bestCost = std::numeric_limits<double>::infinity();
                const auto threshold = std::max(
                    options.maximumFrequencyDistanceHz,
                    peak.frequencyHz
                        * options.maximumRelativeFrequencyDistance);
                const auto firstCandidate = std::ranges::lower_bound(
                    active,
                    peak.frequencyHz - threshold,
                    {},
                    &ActiveTrack::frequencyHz);
                const auto lastCandidate = std::ranges::upper_bound(
                    active,
                    peak.frequencyHz + threshold,
                    {},
                    &ActiveTrack::frequencyHz);
                if (static_cast<std::size_t>(
                        std::distance(firstCandidate, lastCandidate))
                    > maximumPartialTrackTrackingCandidatesPerPeak)
                {
                    return failure(
                        PartialTrackAnalysisErrorCode::LimitExceeded,
                        "Partial-track analysis candidate density exceeds the fixed matching budget",
                        frameIndex);
                }
                for (auto candidate = firstCandidate;
                     candidate != lastCandidate;
                     ++candidate)
                {
                    if (candidate->used)
                        continue;
                    const auto distance = std::abs(
                        peak.frequencyHz - candidate->frequencyHz);
                    const auto amplitudeScale = std::max({
                        peak.amplitude,
                        candidate->amplitude,
                        std::numeric_limits<double>::min()});
                    const auto amplitudeDistance = std::abs(
                        peak.amplitude - candidate->amplitude)
                        / amplitudeScale;
                    if (amplitudeDistance
                        > options.maximumRelativeAmplitudeDistance)
                    {
                        continue;
                    }
                    const auto frequencyCost = threshold == 0.0
                        ? 0.0
                        : distance / threshold;
                    const auto cost = frequencyCost + amplitudeDistance;
                    if (cost < bestCost
                        || (cost == bestCost
                                && (match == nullptr
                                    || candidate->trackId
                                        < match->trackId)))
                    {
                        match = &*candidate;
                        bestCost = cost;
                    }
                }
                if (match == nullptr)
                {
                    if (active.size() + newTracks.size()
                        >= maximumPartialTrackActiveTrackingTracks)
                    {
                        return failure(
                            PartialTrackAnalysisErrorCode::LimitExceeded,
                            "Partial-track analysis active-track count exceeds the fixed matching budget",
                            frameIndex);
                    }
                    if (nextTrackId == 0
                        || nextTrackId
                            > maximumPartialTrackAssetTracks)
                    {
                        return failure(
                            PartialTrackAnalysisErrorCode::LimitExceeded,
                            "Partial-track analysis track count exceeds the fixed limit",
                            frameIndex);
                    }
                    newTracks.push_back({
                        nextTrackId++,
                        source.sourceFrame,
                        peak.frequencyHz,
                        peak.amplitude,
                        true,
                        source.sourceFrame,
                        peak.frequencyHz,
                        peak.amplitude});
                    peak.trackId = newTracks.back().trackId;
                }
                else
                {
                    match->used = true;
                    match->nextSourceFrame = source.sourceFrame;
                    match->nextFrequencyHz = peak.frequencyHz;
                    match->nextAmplitude = peak.amplitude;
                    peak.trackId = match->trackId;
                }
                output.points.push_back(peak);
            }
            for (auto& track : active)
            {
                if (!track.used)
                    continue;
                track.sourceFrame = track.nextSourceFrame;
                track.frequencyHz = track.nextFrequencyHz;
                track.amplitude = track.nextAmplitude;
            }
            active.insert(
                active.end(),
                std::make_move_iterator(newTracks.begin()),
                std::make_move_iterator(newTracks.end()));
            std::ranges::sort(output.points, {}, &PartialTrackPoint::trackId);
            result.asset.frames.push_back(std::move(output));
            previousSegment = source.segmentId;
            previousFrame = source.sourceFrame;
        }
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            PartialTrackAnalysisErrorCode::AllocationFailed,
            "Partial-track analysis allocation failed");
    }
    if (const auto invalid = validatePartialTrackAsset(result.asset))
    {
        return failure(
            PartialTrackAnalysisErrorCode::AnalysisFailed,
            invalid->message,
            invalid->frameIndex);
    }
    result.acceptedFrameCount = result.asset.frames.size();
    result.acceptedPointCount = totalPoints;
    result.progressPermille = 1000;
    return result;
}

PartialTrackAnalysisResult convertLegacyPartialTriples(
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const LegacyPartialTriplesFrame> frames,
    PartialTrackAnalysisOptions options,
    std::function<bool()> shouldCancel)
{
    try
    {
        std::vector<UntrackedPartialFrame> decoded;
        decoded.reserve(frames.size());
        for (std::size_t frameIndex = 0;
             frameIndex < frames.size();
             ++frameIndex)
        {
            if (shouldCancel && shouldCancel())
            {
                return failure(
                    PartialTrackAnalysisErrorCode::Cancelled,
                    "Legacy partial conversion cancelled",
                    frameIndex);
            }
            if (frames[frameIndex].encodedTriples.empty()
                || frames[frameIndex].encodedTriples.size()
                    > 1 + maximumPartialTrackPointsPerFrame * 3)
            {
                return failure(
                    PartialTrackAnalysisErrorCode::InvalidLegacyTriples,
                    "Legacy partial triples are outside the fixed frame bound",
                    frameIndex);
            }
            const auto& encoded = frames[frameIndex].encodedTriples;
            const auto countValue = encoded.front();
            if (!std::isfinite(countValue)
                || countValue < 0.0
                || std::floor(countValue) != countValue
                || countValue
                    > static_cast<double>(
                        maximumPartialTrackPointsPerFrame))
            {
                return failure(
                    PartialTrackAnalysisErrorCode::InvalidLegacyTriples,
                    "Legacy partial triples are malformed",
                    frameIndex);
            }
            const auto count = static_cast<std::size_t>(countValue);
            if (encoded.size() != 1 + count * 3)
            {
                return failure(
                    PartialTrackAnalysisErrorCode::InvalidLegacyTriples,
                    "Legacy partial triples are malformed",
                    frameIndex);
            }
            UntrackedPartialFrame frame{
                frames[frameIndex].segmentId,
                frames[frameIndex].sourceFrame,
                {}};
            frame.peaks.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                const auto base = 1 + index * 3;
                frame.peaks.push_back({
                    0,
                    encoded[base],
                    encoded[base + 1],
                    encoded[base + 2]});
            }
            decoded.push_back(std::move(frame));
        }
        return trackPartialSpectrumFrames(
            sampleRate,
            sourceFrameCount,
            decoded,
            options,
            std::move(shouldCancel));
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            PartialTrackAnalysisErrorCode::AllocationFailed,
            "Legacy partial conversion allocation failed");
    }
}

PartialTrackAnalysisResult assembleCapturedPartialFrames(
    std::uint64_t expectedStageId,
    std::uint64_t expectedBindingGeneration,
    double sampleRate,
    std::uint64_t sourceFrameCount,
    std::span<const CapturedPartialFrame> frames,
    PartialTrackAnalysisOptions options,
    std::function<bool()> shouldCancel)
{
    if (expectedStageId == 0 || expectedBindingGeneration == 0)
    {
        return failure(
            PartialTrackAnalysisErrorCode::InvalidInput,
            "Typed capture requires exact nonzero stage/binding authority");
    }
    try
    {
        bool hasTypedIdentity = false;
        bool hasAnonymousIdentity = false;
        std::uint64_t previousSequence = 0;
        std::size_t totalPoints = 0;
        std::vector<UntrackedPartialFrame> untracked;
        PartialTrackAsset typed{sampleRate, sourceFrameCount, {}};
        untracked.reserve(frames.size());
        typed.frames.reserve(frames.size());
        for (std::size_t frameIndex = 0;
             frameIndex < frames.size();
             ++frameIndex)
        {
            if (shouldCancel && shouldCancel())
            {
                return failure(
                    PartialTrackAnalysisErrorCode::Cancelled,
                    "Typed partial capture assembly cancelled",
                    frameIndex);
            }
            const auto& source = frames[frameIndex];
            if (source.stageId != expectedStageId
                || source.bindingGeneration
                    != expectedBindingGeneration
                || source.sequence == 0
                || (frameIndex != 0
                    && source.sequence <= previousSequence)
                || source.points.size()
                    > maximumPartialTrackPointsPerFrame
                || source.points.size()
                    > maximumPartialTrackAssetPoints - totalPoints)
            {
                return failure(
                    PartialTrackAnalysisErrorCode::InvalidInput,
                    "Typed capture evidence is stale, unordered, or over-bound",
                    frameIndex);
            }
            totalPoints += source.points.size();
            UntrackedPartialFrame anonymous{
                source.segmentId, source.sourceFrame, {}};
            PartialTrackFrame identified{
                source.segmentId, source.sourceFrame, {}};
            anonymous.peaks.reserve(source.points.size());
            identified.points.reserve(source.points.size());
            for (const auto& point : source.points)
            {
                hasTypedIdentity = hasTypedIdentity
                    || point.trackId.has_value();
                hasAnonymousIdentity = hasAnonymousIdentity
                    || !point.trackId.has_value();
                anonymous.peaks.push_back({
                    0,
                    point.amplitude,
                    point.frequencyHz,
                    point.bandwidthHz});
                identified.points.push_back({
                    point.trackId.value_or(0),
                    point.amplitude,
                    point.frequencyHz,
                    point.bandwidthHz});
            }
            untracked.push_back(std::move(anonymous));
            typed.frames.push_back(std::move(identified));
            previousSequence = source.sequence;
        }
        if (hasTypedIdentity && hasAnonymousIdentity)
        {
            return failure(
                PartialTrackAnalysisErrorCode::InvalidInput,
                "One typed capture cannot mix tracked and anonymous point identity");
        }
        if (!hasTypedIdentity)
        {
            return trackPartialSpectrumFrames(
                sampleRate,
                sourceFrameCount,
                untracked,
                options,
                std::move(shouldCancel));
        }
        if (const auto invalid = validatePartialTrackAsset(typed))
        {
            return failure(
                PartialTrackAnalysisErrorCode::AnalysisFailed,
                invalid->message,
                invalid->frameIndex);
        }
        PartialTrackAnalysisResult result;
        result.asset = std::move(typed);
        result.acceptedFrameCount = result.asset.frames.size();
        result.acceptedPointCount = totalPoints;
        result.progressPermille = 1000;
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            PartialTrackAnalysisErrorCode::AllocationFailed,
            "Typed partial capture assembly allocation failed");
    }
}

}
