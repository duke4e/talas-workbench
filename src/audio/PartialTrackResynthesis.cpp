#include "talas/audio/PartialTrackResynthesis.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <ranges>
#include <unordered_map>

namespace talas::audio
{
namespace
{
[[nodiscard]] bool validProfile(
    const PartialComplexAdapterProfile& profile) noexcept
{
    return profile.frameSize >= 8
        && profile.frameSize <= 65536
        && std::has_single_bit(profile.frameSize)
        && profile.hopSize != 0
        && profile.hopSize <= profile.frameSize
        && std::isfinite(profile.sampleRate)
        && profile.sampleRate > 0.0
        && profile.maximumPartials != 0
        && profile.maximumPartials
            <= talas::project::maximumPartialTrackPointsPerFrame;
}

[[nodiscard]] bool validPartial(
    const talas::project::PartialTrackPoint& point,
    double sampleRate) noexcept
{
    return point.trackId != 0
        && std::isfinite(point.amplitude)
        && point.amplitude >= 0.0
        && std::isfinite(point.frequencyHz)
        && point.frequencyHz > 0.0
        && point.frequencyHz <= sampleRate * 0.5
        && std::isfinite(point.bandwidthHz)
        && point.bandwidthHz >= 0.0;
}

[[nodiscard]] double deterministicBandwidthNoise(
    std::uint64_t segmentId,
    std::uint64_t trackId,
    std::uint64_t frame) noexcept
{
    auto bits = frame
        ^ (trackId * 0x9e3779b97f4a7c15ULL)
        ^ (segmentId * 0xbf58476d1ce4e5b9ULL);
    bits ^= bits >> 30U;
    bits *= 0xbf58476d1ce4e5b9ULL;
    bits ^= bits >> 27U;
    bits *= 0x94d049bb133111ebULL;
    bits ^= bits >> 31U;
    constexpr double unit = 1.0 / 9007199254740992.0;
    return 2.0 * static_cast<double>(bits >> 11U) * unit - 1.0;
}
}

PreparedPartialTrackResynthesizer::PreparedPartialTrackResynthesizer(
    const talas::project::PartialTrackAsset& asset,
    std::span<const talas::project::PartialTrackPointInterpolation>
        interpolations,
    PartialTrackResynthesisProfile profile)
{
    if (talas::project::validatePartialTrackAsset(asset).has_value()
        || profile.maximumActiveTracks == 0
        || profile.maximumActiveTracks
            > talas::project::maximumPartialTrackAssetTracks
        || !std::isfinite(profile.outputGain)
        || profile.outputGain < 0.0)
    {
        return;
    }
    std::size_t totalPoints = 0;
    for (const auto& frame : asset.frames)
        totalPoints += frame.points.size();
    if (asset.loopStartFrame.has_value() && asset.loopEndFrame.has_value())
    {
        loopStart_ = *asset.loopStartFrame;
        loopEnd_ = *asset.loopEndFrame;
    }
    if (!interpolations.empty()
        && interpolations.size() != totalPoints)
    {
        return;
    }

    try
    {
        std::unordered_map<std::uint64_t, std::size_t> trackIndices;
        trackIndices.reserve(std::min(
            totalPoints, profile.maximumActiveTracks));
        std::size_t interpolationIndex = 0;
        for (const auto& frame : asset.frames)
        {
            if (segments_.empty()
                || segments_.back().segmentId != frame.segmentId)
            {
                if (!segments_.empty())
                    segments_.back().endFrame = frame.sourceFrame;
                segments_.push_back({
                    frame.segmentId,
                    frame.sourceFrame,
                    asset.sourceFrameCount,
                    {},
                });
                trackIndices.clear();
            }
            const auto includeSegment = profile.segmentId == 0
                || frame.segmentId == profile.segmentId;
            for (const auto& point : frame.points)
            {
                const auto interpolation = interpolations.empty()
                    ? talas::project::PartialTrackPointInterpolation::Linear
                    : interpolations[interpolationIndex];
                ++interpolationIndex;
                if (!includeSegment)
                    continue;
                auto& tracks = segments_.back().tracks;
                auto found = trackIndices.find(point.trackId);
                if (found == trackIndices.end())
                {
                    if (tracks.size() >= profile.maximumActiveTracks)
                        return;
                    const auto index = tracks.size();
                    tracks.push_back({point.trackId, {}});
                    trackIndices.emplace(point.trackId, index);
                    found = trackIndices.find(point.trackId);
                }
                tracks[found->second].points.push_back({
                    frame.sourceFrame,
                    point.amplitude,
                    point.frequencyHz,
                    point.bandwidthHz,
                    interpolation});
            }
        }
        if (segments_.empty())
            return;
        if (profile.segmentId != 0)
        {
            const auto selected = std::ranges::find(
                segments_,
                profile.segmentId,
                &PreparedSegment::segmentId);
            if (selected == segments_.end())
                return;
            auto retained = std::move(*selected);
            segments_.clear();
            segments_.push_back(std::move(retained));
        }
        std::size_t maximumActiveTracks = 0;
        for (auto& segment : segments_)
        {
            std::ranges::sort(segment.tracks, {}, &PreparedTrack::trackId);
            totalTrackCount_ += segment.tracks.size();
            maximumActiveTracks = std::max(maximumActiveTracks, segment.tracks.size());
            for (const auto& track : segment.tracks)
            {
                if (!std::ranges::is_sorted(
                        track.points,
                        {},
                        &PreparedPoint::frame))
                {
                    return;
                }
            }
        }
        activeTracks_.resize(maximumActiveTracks);
        preparationIdentity_ = std::make_shared<const PreparationIdentity>();
    }
    catch (...)
    {
        segments_.clear();
        activeTracks_.clear();
        preparationIdentity_.reset();
        totalTrackCount_ = 0;
        return;
    }
    sampleRate_ = asset.sampleRate;
    sourceFrameCount_ = asset.sourceFrameCount;
    outputGain_ = profile.outputGain;
    ready_ = true;
}

bool PreparedPartialTrackResynthesizer::ready() const noexcept
{
    return ready_;
}

double PreparedPartialTrackResynthesizer::sampleRate() const noexcept
{
    return sampleRate_;
}

std::uint64_t
PreparedPartialTrackResynthesizer::sourceFrameCount() const noexcept
{
    return sourceFrameCount_;
}

std::size_t PreparedPartialTrackResynthesizer::trackCount() const noexcept
{
    return totalTrackCount_;
}

std::unique_ptr<PreparedPartialTrackResynthesizer>
PreparedPartialTrackResynthesizer::cloneProcessingState() const
{
    // The private copy constructor keeps ordinary copying unavailable while
    // this explicit offline/control-thread operation duplicates all owned state.
    return std::unique_ptr<PreparedPartialTrackResynthesizer>(new PreparedPartialTrackResynthesizer(*this));
}

void PreparedPartialTrackResynthesizer::reset() noexcept
{
    std::ranges::fill(activeTracks_, TrackContinuation{});
    currentSegmentIndex_ = 0;
    nextSourceFrame_ = 0;
    initialized_ = false;
}

std::unique_ptr<PreparedPartialTrackResynthesizer::ProcessingCheckpoint>
PreparedPartialTrackResynthesizer::createProcessingCheckpoint() const
{
    if (!ready_ || !preparationIdentity_)
        return {};
    auto checkpoint = std::make_unique<ProcessingCheckpoint>();
    checkpoint->preparationIdentity_ = preparationIdentity_;
    checkpoint->activeTracks_.resize(activeTracks_.size());
    if (!captureProcessingState(*checkpoint))
        return {};
    return checkpoint;
}

bool PreparedPartialTrackResynthesizer::captureProcessingState(
    ProcessingCheckpoint& checkpoint) const noexcept
{
    if (!ready_ || !preparationIdentity_
        || checkpoint.preparationIdentity_ != preparationIdentity_
        || checkpoint.activeTracks_.size() != activeTracks_.size())
    {
        return false;
    }
    std::ranges::copy(activeTracks_, checkpoint.activeTracks_.begin());
    checkpoint.currentSegmentIndex_ = currentSegmentIndex_;
    checkpoint.nextSourceFrame_ = nextSourceFrame_;
    checkpoint.initialized_ = initialized_;
    return true;
}

bool PreparedPartialTrackResynthesizer::restoreProcessingState(
    const ProcessingCheckpoint& checkpoint) noexcept
{
    if (!ready_ || !preparationIdentity_
        || checkpoint.preparationIdentity_ != preparationIdentity_
        || checkpoint.activeTracks_.size() != activeTracks_.size())
    {
        return false;
    }
    std::ranges::copy(checkpoint.activeTracks_, activeTracks_.begin());
    currentSegmentIndex_ = checkpoint.currentSegmentIndex_;
    nextSourceFrame_ = checkpoint.nextSourceFrame_;
    initialized_ = checkpoint.initialized_;
    return true;
}

PartialTrackResynthesisStatus
PreparedPartialTrackResynthesizer::render(
    std::uint64_t sourceStartFrame,
    std::span<double> output) noexcept
{
    if (!ready_)
        return PartialTrackResynthesisStatus::InvalidPreparation;
    if (output.size()
        > std::numeric_limits<std::uint64_t>::max()
            - sourceStartFrame)
    {
        return PartialTrackResynthesisStatus::FrameOverflow;
    }
    if (!initialized_ || sourceStartFrame != nextSourceFrame_)
    {
        reset();
        initialized_ = true;
    }
    std::ranges::fill(output, 0.0);
    for (std::size_t sample = 0; sample < output.size(); ++sample)
    {
        auto frame = sourceStartFrame + sample;
        // Past the end loop marker the analysis plays again
        // from the start loop marker, each track from its first point there.
        if (loopEnd_ > loopStart_ && frame >= loopEnd_)
        {
            frame = loopStart_ + (frame - loopStart_) % (loopEnd_ - loopStart_);
            if (frame == loopStart_)
            {
                currentSegmentIndex_ = 0;
                for (auto& continuation : activeTracks_)
                    continuation.cursor = 0;
            }
        }
        if (frame >= sourceFrameCount_)
            continue;
        const auto priorSegmentIndex = currentSegmentIndex_;
        while (currentSegmentIndex_ + 1 < segments_.size()
               && segments_[currentSegmentIndex_ + 1].startFrame <= frame)
        {
            ++currentSegmentIndex_;
        }
        if (currentSegmentIndex_ != priorSegmentIndex)
            std::ranges::fill(activeTracks_, TrackContinuation{});
        const auto& segment = segments_[currentSegmentIndex_];
        if (frame < segment.startFrame || frame >= segment.endFrame)
            continue;
        double mixed = 0.0;
        for (std::size_t trackIndex = 0; trackIndex < segment.tracks.size(); ++trackIndex)
        {
            const auto& track = segment.tracks[trackIndex];
            auto& continuation = activeTracks_[trackIndex];
            while (continuation.cursor + 1 < track.points.size()
                   && track.points[continuation.cursor + 1].frame <= frame)
            {
                ++continuation.cursor;
            }
            if (track.points.empty()
                || frame < track.points.front().frame)
            {
                continue;
            }
            const auto& left = track.points[continuation.cursor];
            double amplitude = left.amplitude;
            double frequency = left.frequencyHz;
            double bandwidth = left.bandwidthHz;
            if (left.interpolation
                    == talas::project::PartialTrackPointInterpolation::Linear
                && continuation.cursor + 1 < track.points.size())
            {
                const auto& right = track.points[continuation.cursor + 1];
                if (right.frame > left.frame && frame < right.frame)
                {
                    const auto ratio = static_cast<double>(
                        frame - left.frame)
                        / static_cast<double>(right.frame - left.frame);
                    amplitude = std::lerp(
                        left.amplitude, right.amplitude, ratio);
                    frequency = std::lerp(
                        left.frequencyHz, right.frequencyHz, ratio);
                    bandwidth = std::lerp(
                        left.bandwidthHz, right.bandwidthHz, ratio);
                }
            }
            const auto noiseMix = std::clamp(
                bandwidth / std::max(1.0, frequency), 0.0, 1.0);
            mixed += amplitude
                * ((1.0 - noiseMix) * std::sin(continuation.phase)
                    + noiseMix * deterministicBandwidthNoise(
                        segment.segmentId, track.trackId, frame));
            continuation.phase = std::fmod(
                continuation.phase
                    + 2.0 * std::numbers::pi
                        * frequency / sampleRate_,
                2.0 * std::numbers::pi);
        }
        output[sample] = mixed * outputGain_;
    }
    nextSourceFrame_ = sourceStartFrame + output.size();
    initialized_ = true;
    return PartialTrackResynthesisStatus::Ok;
}

PartialTrackOfflineRenderResult renderPartialTrackAssetOffline(
    const talas::project::PartialTrackAsset& asset,
    std::span<const talas::project::PartialTrackPointInterpolation>
        interpolations,
    PartialTrackResynthesisProfile profile,
    std::size_t blockFrames) noexcept
{
    PartialTrackOfflineRenderResult result;
    if (blockFrames == 0
        || asset.sourceFrameCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        result.status = PartialTrackResynthesisStatus::InvalidRenderSpan;
        return result;
    }
    try
    {
        PreparedPartialTrackResynthesizer synthesizer{
            asset, interpolations, profile};
        if (!synthesizer.ready())
            return result;
        result.samples.resize(
            static_cast<std::size_t>(asset.sourceFrameCount));
        std::size_t offset = 0;
        while (offset < result.samples.size())
        {
            const auto count = std::min(
                blockFrames, result.samples.size() - offset);
            result.status = synthesizer.render(
                static_cast<std::uint64_t>(offset),
                std::span<double>{result.samples}.subspan(offset, count));
            if (result.status != PartialTrackResynthesisStatus::Ok)
            {
                result.samples.clear();
                return result;
            }
            offset += count;
        }
        result.status = PartialTrackResynthesisStatus::Ok;
    }
    catch (...)
    {
        result.samples.clear();
        result.status = PartialTrackResynthesisStatus::InvalidPreparation;
    }
    return result;
}

PartialComplexConversionInfo partialComplexConversionInfo(
    const PartialComplexAdapterProfile& profile) noexcept
{
    return {
        partialComplexAdapterVersion,
        validProfile(profile) ? profile.frameSize : 0,
        true,
        true,
        true,
    };
}

PartialComplexConversionStatus partialsToComplexBins(
    std::span<const talas::project::PartialTrackPoint> partials,
    const PartialComplexAdapterProfile& profile,
    std::span<std::complex<double>> bins) noexcept
{
    if (!validProfile(profile))
        return PartialComplexConversionStatus::InvalidProfile;
    const auto requiredBins = profile.frameSize / 2 + 1;
    if (bins.size() < requiredBins)
        return PartialComplexConversionStatus::DestinationTooSmall;
    if (partials.size() > profile.maximumPartials
        || !std::ranges::all_of(
            partials,
            [&](const auto& point) {
                return validPartial(point, profile.sampleRate);
            }))
    {
        return PartialComplexConversionStatus::InvalidInput;
    }
    std::ranges::fill(
        bins.first(requiredBins), std::complex<double>{});
    const auto binHz = profile.sampleRate
        / static_cast<double>(profile.frameSize);
    for (const auto& point : partials)
    {
        const auto bin = static_cast<std::size_t>(std::min(
            static_cast<double>(requiredBins - 1),
            std::floor(point.frequencyHz / binHz + 0.5)));
        bins[bin] += std::complex<double>{point.amplitude, 0.0};
    }
    return PartialComplexConversionStatus::Ok;
}

PartialComplexConversionStatus complexBinsToPartials(
    std::span<const std::complex<double>> bins,
    const PartialComplexAdapterProfile& profile,
    std::span<talas::project::PartialTrackPoint> partials,
    std::size_t& writtenPartials) noexcept
{
    writtenPartials = 0;
    if (!validProfile(profile))
        return PartialComplexConversionStatus::InvalidProfile;
    const auto requiredBins = profile.frameSize / 2 + 1;
    if (bins.size() != requiredBins
        || !std::ranges::all_of(bins, [](const auto& value) {
               return std::isfinite(value.real())
                   && std::isfinite(value.imag());
           }))
    {
        return PartialComplexConversionStatus::InvalidInput;
    }
    const auto binHz = profile.sampleRate
        / static_cast<double>(profile.frameSize);
    std::size_t peakCount = 0;
    for (std::size_t bin = 1;
         bin + 1 < bins.size();
         ++bin)
    {
        const auto magnitude = std::abs(bins[bin]);
        if (magnitude > 0.0
            && magnitude >= std::abs(bins[bin - 1])
            && magnitude >= std::abs(bins[bin + 1]))
        {
            ++peakCount;
        }
    }
    if (peakCount > partials.size()
        || peakCount > profile.maximumPartials)
    {
        return PartialComplexConversionStatus::DestinationTooSmall;
    }
    for (std::size_t bin = 1;
         bin + 1 < bins.size();
         ++bin)
    {
        const auto magnitude = std::abs(bins[bin]);
        if (magnitude <= 0.0
            || magnitude < std::abs(bins[bin - 1])
            || magnitude < std::abs(bins[bin + 1]))
        {
            continue;
        }
        partials[writtenPartials] = {
            0,
            magnitude,
            static_cast<double>(bin) * binHz,
            binHz};
        ++writtenPartials;
    }
    return PartialComplexConversionStatus::Ok;
}
}
