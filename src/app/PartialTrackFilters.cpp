#include "talas/app/PartialTrackFilters.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace talas::app
{
namespace
{
using talas::project::MaterializedPartialTrackEdits;
using talas::project::ProjectPartialTrackPointMutation;
using talas::project::ProjectPartialTrackPointMutationKind;
using talas::project::ProjectPartialTrackStateMutation;

struct TrackPoint
{
    std::uint64_t segmentId = 0;
    std::uint64_t frame = 0;
    talas::project::PartialTrackPoint point;
    talas::project::MaterializedPartialTrackPointIdentity identity;
    talas::project::PartialTrackPointInterpolation interpolation =
        talas::project::PartialTrackPointInterpolation::Linear;
};

// Each track's points in time order, the materialized identities and
// interpolations running alongside the frames' points.
[[nodiscard]] std::map<std::uint64_t, std::vector<TrackPoint>> pointsByTrack(
    const MaterializedPartialTrackEdits& materialized)
{
    std::map<std::uint64_t, std::vector<TrackPoint>> tracks;
    std::size_t flat = 0;
    for (const auto& frame : materialized.asset.frames)
    {
        for (const auto& point : frame.points)
        {
            TrackPoint entry{frame.segmentId, frame.sourceFrame, point, {}, {}};
            if (flat < materialized.pointIdentities.size())
                entry.identity = materialized.pointIdentities[flat];
            if (flat < materialized.pointInterpolations.size())
                entry.interpolation = materialized.pointInterpolations[flat];
            tracks[point.trackId].push_back(std::move(entry));
            ++flat;
        }
    }
    for (auto& [_, points] : tracks)
    {
        std::ranges::stable_sort(points, [](const TrackPoint& left, const TrackPoint& right) {
            return std::pair{left.segmentId, left.frame} < std::pair{right.segmentId, right.frame};
        });
    }
    return tracks;
}

[[nodiscard]] double mean(std::span<const double> values)
{
    double sum = 0.0;
    for (const auto value : values)
        sum += value;
    return values.empty() ? 0.0 : sum / static_cast<double>(values.size());
}

[[nodiscard]] double median(std::vector<double> values)
{
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

// Each value replaced by the mean or median of the neighborhood around it,
// the neighborhood cut short at the region's ends.
[[nodiscard]] std::vector<double> neighborhoods(
    std::span<const double> values,
    std::size_t size,
    bool takeMedian)
{
    std::vector<double> result(values.size());
    const auto before = (size - 1) / 2;
    const auto after = size / 2;
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const auto first = index < before ? 0 : index - before;
        const auto last = std::min(values.size() - 1, index + after);
        const auto window = values.subspan(first, last - first + 1);
        result[index] = takeMedian ? median({window.begin(), window.end()}) : mean(window);
    }
    return result;
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

// A frequency deviation that wanders to a new random value within plus or
// minus the deviation so many times a second, gliding between them.
[[nodiscard]] double deviationAt(
    double seconds,
    const PartialTrackFilterSettings& settings,
    std::uint64_t trackId)
{
    const auto position = seconds * settings.changesPerSecond;
    const auto step = static_cast<std::uint64_t>(std::floor(position));
    const auto valueAt = [&](std::uint64_t index) {
        auto state = settings.seed * 0x2545f4914f6cdd1dULL + trackId * 0x9e3779b97f4a7c15ULL + index;
        const auto unit = static_cast<double>(nextRandom(state) >> 11U) / static_cast<double>(1ULL << 53U);
        return (unit * 2.0 - 1.0) * settings.deviationHz;
    };
    const auto fraction = position - static_cast<double>(step);
    return valueAt(step) + (valueAt(step + 1) - valueAt(step)) * fraction;
}

[[nodiscard]] double frequencyAt(std::span<const TrackPoint> guide, std::uint64_t segmentId, std::uint64_t frame)
{
    const auto key = std::pair{segmentId, frame};
    const auto after = std::ranges::find_if(guide, [&](const TrackPoint& point) {
        return std::pair{point.segmentId, point.frame} >= key;
    });
    if (after == guide.begin())
        return after->point.frequencyHz;
    if (after == guide.end())
        return guide.back().point.frequencyHz;
    const auto& previous = *std::prev(after);
    if (after->segmentId != previous.segmentId || after->frame == previous.frame)
        return after->point.frequencyHz;
    const auto fraction = static_cast<double>(frame - previous.frame) / static_cast<double>(after->frame - previous.frame);
    return previous.point.frequencyHz + (after->point.frequencyHz - previous.point.frequencyHz) * fraction;
}
}

std::vector<ProjectPartialTrackPointMutation> clearPartialTrackPoints(
    const MaterializedPartialTrackEdits& materialized,
    const std::set<std::pair<std::uint64_t, std::uint64_t>>& points)
{
    std::vector<ProjectPartialTrackPointMutation> mutations;
    for (const auto& [trackId, trackPoints] : pointsByTrack(materialized))
    {
        for (const auto& point : trackPoints)
        {
            if (!points.contains({trackId, point.frame}) || point.point.amplitude == 0.0)
                continue;
            auto output = point.point;
            output.amplitude = 0.0;
            mutations.push_back({ProjectPartialTrackPointMutationKind::Update, point.identity, point.segmentId, point.frame,
                output, point.interpolation});
        }
    }
    return mutations;
}

PartialTrackFilterResult filterPartialTracks(
    const MaterializedPartialTrackEdits& materialized,
    const PartialTrackFilterRegion& region,
    PartialTrackFilter filter,
    const PartialTrackFilterSettings& settings)
{
    PartialTrackFilterResult result;
    const auto fail = [&](std::string message) {
        result.mutations.clear();
        result.error = std::move(message);
        return result;
    };
    const auto smoothing = filter == PartialTrackFilter::SmoothAmplitude
        || filter == PartialTrackFilter::RemoveAmplitudeOutliers || filter == PartialTrackFilter::SmoothFrequency
        || filter == PartialTrackFilter::RemoveFrequencyOutliers;
    if (smoothing && (settings.neighborhood == 0 || settings.neighborhood > 1024))
        return fail("The neighborhood must hold between 1 and 1024 values");
    if (filter == PartialTrackFilter::ScaleAmplitudes && !(std::isfinite(settings.scale) && settings.scale >= 0.0))
        return fail("Amplitudes scale by a finite amount of zero or more");
    if (filter == PartialTrackFilter::AddFrequencyNoise
        && !(std::isfinite(settings.deviationHz) && settings.deviationHz >= 0.0 && std::isfinite(settings.changesPerSecond)
            && settings.changesPerSecond > 0.0))
        return fail("Noise takes a deviation of zero or more hertz and a rate above zero");
    if (filter == PartialTrackFilter::Arpeggiate && !(std::isfinite(settings.delaySeconds) && settings.delaySeconds >= 0.0))
        return fail("Arpeggiate delays each track by zero or more seconds");
    if (region.startFrame > region.endFrame)
        return fail("The region ends before it starts");

    const auto tracks = pointsByTrack(materialized);
    const auto nyquist = materialized.asset.sampleRate * 0.5;
    std::set<std::uint64_t> selected(region.trackIds.begin(), region.trackIds.end());
    const std::vector<TrackPoint>* guide = nullptr;
    if (filter == PartialTrackFilter::ForceHarmonicFrequency)
    {
        const auto found = tracks.find(settings.guideTrack);
        if (settings.guideTrack == 0 || found == tracks.end() || found->second.empty())
            return fail("Guide track " + std::to_string(settings.guideTrack) + " has no points");
        guide = &found->second;
    }
    std::size_t order = 0;
    for (const auto trackId : selected)
    {
        const auto found = tracks.find(trackId);
        if (found == tracks.end())
            continue;
        std::vector<TrackPoint> inRegion;
        for (const auto& point : found->second)
        {
            if (point.frame >= region.startFrame && point.frame <= region.endFrame)
                inRegion.push_back(point);
        }
        if (inRegion.empty())
            continue;
        std::vector<double> amplitudes;
        std::vector<double> frequencies;
        for (const auto& point : inRegion)
        {
            amplitudes.push_back(point.point.amplitude);
            frequencies.push_back(point.point.frequencyHz);
        }
        std::uint64_t delayFrames = 0;
        switch (filter)
        {
        case PartialTrackFilter::ScaleAmplitudes:
            for (auto& value : amplitudes)
                value *= settings.scale;
            break;
        case PartialTrackFilter::AverageAmplitude:
            std::ranges::fill(amplitudes, mean(amplitudes));
            break;
        case PartialTrackFilter::SmoothAmplitude:
        case PartialTrackFilter::RemoveAmplitudeOutliers:
            amplitudes = neighborhoods(amplitudes, settings.neighborhood, filter == PartialTrackFilter::RemoveAmplitudeOutliers);
            break;
        case PartialTrackFilter::ForceHarmonicFrequency:
            if (trackId == settings.guideTrack)
                continue;
            for (std::size_t index = 0; index < inRegion.size(); ++index)
            {
                frequencies[index] = frequencyAt(*guide, inRegion[index].segmentId, inRegion[index].frame)
                    * static_cast<double>(trackId) / static_cast<double>(settings.guideTrack);
            }
            break;
        case PartialTrackFilter::AverageFrequency:
            std::ranges::fill(frequencies, mean(frequencies));
            break;
        case PartialTrackFilter::SmoothFrequency:
        case PartialTrackFilter::RemoveFrequencyOutliers:
            frequencies = neighborhoods(frequencies, settings.neighborhood, filter == PartialTrackFilter::RemoveFrequencyOutliers);
            break;
        case PartialTrackFilter::AddFrequencyNoise:
            for (std::size_t index = 0; index < inRegion.size(); ++index)
            {
                const auto seconds = static_cast<double>(inRegion[index].frame) / materialized.asset.sampleRate;
                frequencies[index] = std::clamp(frequencies[index] + deviationAt(seconds, settings, trackId), 1.0e-3, nyquist);
            }
            break;
        case PartialTrackFilter::Arpeggiate:
            // Higher numbered tracks wait proportionally longer: each one
            // the delay after the one before it.
            delayFrames = static_cast<std::uint64_t>(
                std::llround(settings.delaySeconds * materialized.asset.sampleRate * static_cast<double>(order)));
            break;
        }
        ++order;
        for (std::size_t index = 0; index < inRegion.size(); ++index)
        {
            const auto& point = inRegion[index];
            if (!(std::isfinite(frequencies[index]) && frequencies[index] > 0.0 && frequencies[index] <= nyquist))
                return fail("Track " + std::to_string(trackId) + " would leave the frequencies below half the sample rate");
            if (delayFrames > 0 && (point.frame > materialized.asset.sourceFrameCount - 1
                    || delayFrames > materialized.asset.sourceFrameCount - 1 - point.frame))
                return fail("Track " + std::to_string(trackId) + " would be delayed past the end of the analysis");
            auto output = point.point;
            output.amplitude = amplitudes[index] == 0.0 ? 0.0 : amplitudes[index];
            output.frequencyHz = frequencies[index];
            result.mutations.push_back({
                ProjectPartialTrackPointMutationKind::Update,
                point.identity,
                point.segmentId,
                point.frame + delayFrames,
                output,
                point.interpolation,
            });
            if (result.mutations.size() > talas::project::maximumProjectPartialTrackBatchPointMutations)
                return fail("The selection holds more points than one edit may change");
        }
    }
    if (result.mutations.empty())
        return fail("No point of the selected tracks lies in the region");
    return result;
}

std::vector<ProjectPartialTrackStateMutation> clearPartialTracks(
    const MaterializedPartialTrackEdits& materialized,
    std::span<const std::uint64_t> trackIds)
{
    const auto tracks = pointsByTrack(materialized);
    std::vector<ProjectPartialTrackStateMutation> mutations;
    for (const auto trackId : std::set<std::uint64_t>(trackIds.begin(), trackIds.end()))
    {
        if (tracks.contains(trackId))
            mutations.push_back({trackId, true, false});
    }
    return mutations;
}

std::vector<ProjectPartialTrackStateMutation> trimPartialTracks(
    const MaterializedPartialTrackEdits& materialized,
    std::span<const std::uint64_t> trackIds)
{
    const std::set<std::uint64_t> kept(trackIds.begin(), trackIds.end());
    std::vector<ProjectPartialTrackStateMutation> mutations;
    for (const auto& [trackId, _] : pointsByTrack(materialized))
    {
        if (!kept.contains(trackId))
            mutations.push_back({trackId, true, false});
    }
    return mutations;
}
}
