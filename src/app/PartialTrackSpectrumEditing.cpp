#include "talas/app/PartialTrackSpectrumEditing.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <tuple>

namespace talas::app
{
namespace
{
using talas::project::MaterializedPartialTrackEdits;

// Every point with its frame, in the materialized order.
template <typename Visit>
void forEachPoint(const MaterializedPartialTrackEdits& materialized, Visit&& visit)
{
    std::size_t flat = 0;
    for (const auto& frame : materialized.asset.frames)
    {
        for (const auto& point : frame.points)
            visit(frame, point, flat++);
    }
}

[[nodiscard]] bool isPrime(std::uint64_t number) noexcept
{
    if (number < 2)
        return false;
    for (std::uint64_t divisor = 2; divisor * divisor <= number; ++divisor)
    {
        if (number % divisor == 0)
            return false;
    }
    return true;
}

[[nodiscard]] bool isFibonacci(std::uint64_t number) noexcept
{
    std::uint64_t previous = 1;
    std::uint64_t current = 2;
    if (number == 1 || number == 2)
        return true;
    while (current < number)
    {
        const auto next = previous + current;
        previous = current;
        current = next;
    }
    return current == number;
}

// The points kept, their frames moved by `shift(frame)`, as one
// materialized spectrum.
template <typename Keep, typename Shift>
[[nodiscard]] std::optional<MaterializedPartialTrackEdits> retained(
    const MaterializedPartialTrackEdits& materialized, Keep&& keep, Shift&& shift)
{
    MaterializedPartialTrackEdits result;
    result.asset.sampleRate = materialized.asset.sampleRate;
    result.trackStates = materialized.trackStates;
    std::size_t flat = 0;
    std::uint64_t last = 0;
    for (const auto& frame : materialized.asset.frames)
    {
        talas::project::PartialTrackFrame kept{frame.segmentId, 0, {}};
        bool any = false;
        for (const auto& point : frame.points)
        {
            if (keep(frame.sourceFrame, point))
            {
                kept.points.push_back(point);
                if (flat < materialized.pointInterpolations.size())
                    result.pointInterpolations.push_back(materialized.pointInterpolations[flat]);
                if (flat < materialized.pointIdentities.size())
                    result.pointIdentities.push_back(materialized.pointIdentities[flat]);
                any = true;
            }
            ++flat;
        }
        if (any)
        {
            kept.sourceFrame = shift(frame.sourceFrame);
            last = std::max(last, kept.sourceFrame);
            result.asset.frames.push_back(std::move(kept));
        }
    }
    if (result.asset.frames.empty())
        return std::nullopt;
    result.asset.sourceFrameCount = last + 1;
    return result;
}
}

void PartialTrackSelection::selectBox(
    const MaterializedPartialTrackEdits& materialized,
    PartialTrackBox box,
    bool shift,
    std::uint64_t startTrack,
    std::uint64_t startFrame)
{
    if (box.firstTrack > box.lastTrack)
        std::swap(box.firstTrack, box.lastTrack);
    if (box.startFrame > box.endFrame)
        std::swap(box.startFrame, box.endFrame);
    const auto cutsHole = shift && inside(startTrack, startFrame);
    if (!shift)
        points_.clear();
    forEachPoint(materialized, [&](const auto& frame, const auto& point, std::size_t) {
        if (point.trackId < box.firstTrack || point.trackId > box.lastTrack || frame.sourceFrame < box.startFrame
            || frame.sourceFrame > box.endFrame)
            return;
        if (cutsHole)
            points_.erase({point.trackId, frame.sourceFrame});
        else
            points_.insert({point.trackId, frame.sourceFrame});
    });
}

bool PartialTrackSelection::inside(std::uint64_t trackId, std::uint64_t frame) const
{
    const auto after = points_.lower_bound({trackId, frame});
    if (after != points_.end() && after->first == trackId && after->second == frame)
        return true;
    const auto before = after == points_.begin() ? points_.end() : std::prev(after);
    return after != points_.end() && after->first == trackId && before != points_.end() && before->first == trackId;
}

void PartialTrackSelection::selectTrack(const MaterializedPartialTrackEdits& materialized, std::uint64_t trackId, bool shift)
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> track;
    forEachPoint(materialized, [&](const auto& frame, const auto& point, std::size_t) {
        if (point.trackId == trackId)
            track.emplace_back(trackId, frame.sourceFrame);
    });
    const auto whollySelected = !track.empty()
        && std::ranges::all_of(track, [&](const auto& key) { return points_.contains(key); });
    if (!shift)
        points_.clear();
    for (const auto& key : track)
    {
        if (shift && whollySelected)
            points_.erase(key);
        else
            points_.insert(key);
    }
}

void PartialTrackSelection::selectComplement(const MaterializedPartialTrackEdits& materialized)
{
    std::set<std::pair<std::uint64_t, std::uint64_t>> complement;
    forEachPoint(materialized, [&](const auto& frame, const auto& point, std::size_t) {
        if (!points_.contains({point.trackId, frame.sourceFrame}))
            complement.insert({point.trackId, frame.sourceFrame});
    });
    points_ = std::move(complement);
}

void PartialTrackSelection::selectAll(const MaterializedPartialTrackEdits& materialized)
{
    points_.clear();
    selectComplement(materialized);
}

void PartialTrackSelection::add(std::uint64_t trackId, std::uint64_t frame)
{
    points_.insert({trackId, frame});
}

void PartialTrackSelection::selectPoints(
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& points, bool shift, bool startsInside)
{
    if (!shift)
        points_.clear();
    for (const auto& key : points)
    {
        if (shift && startsInside)
            points_.erase(key);
        else
            points_.insert(key);
    }
}

void PartialTrackSelection::clear() noexcept
{
    points_.clear();
}

bool PartialTrackSelection::contains(std::uint64_t trackId, std::uint64_t frame) const
{
    return points_.contains({trackId, frame});
}

bool PartialTrackSelection::empty() const noexcept
{
    return points_.empty();
}

std::vector<std::uint64_t> PartialTrackSelection::trackIds() const
{
    std::vector<std::uint64_t> tracks;
    for (const auto& [trackId, _] : points_)
    {
        if (tracks.empty() || tracks.back() != trackId)
            tracks.push_back(trackId);
    }
    return tracks;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> PartialTrackSelection::frameSpan() const
{
    if (points_.empty())
        return std::nullopt;
    auto first = points_.begin()->second;
    auto last = first;
    for (const auto& [_, frame] : points_)
    {
        first = std::min(first, frame);
        last = std::max(last, frame);
    }
    return std::pair{first, last};
}

const std::set<std::pair<std::uint64_t, std::uint64_t>>& PartialTrackSelection::points() const noexcept
{
    return points_;
}

std::optional<std::uint64_t> partialTrackForNumberKey(char key) noexcept
{
    if (key == '0')
        return 10;
    if (key >= '1' && key <= '9')
        return static_cast<std::uint64_t>(key - '0');
    return std::nullopt;
}

int partialTrackRawAmplitude(double amplitude) noexcept
{
    if (!std::isfinite(amplitude))
        return 0;
    return static_cast<int>(std::lround(std::clamp(amplitude, 0.0, 1.0) * 127.0));
}

PartialTrackSelection selectPartialTracksBy(
    const MaterializedPartialTrackEdits& materialized, PartialTrackSelectionCriterion criterion, int lowRaw, int highRaw)
{
    PartialTrackSelection selection;
    const auto chosen = [&](const talas::project::PartialTrackPoint& point) {
        const auto raw = partialTrackRawAmplitude(point.amplitude);
        const auto track = point.trackId;
        switch (criterion)
        {
        case PartialTrackSelectionCriterion::AmplitudesWithinRange:
            return raw >= lowRaw && raw <= highRaw;
        case PartialTrackSelectionCriterion::AboveAmplitudeThreshold:
            return raw > lowRaw;
        case PartialTrackSelectionCriterion::EvenTracks:
            return track % 2 == 0;
        case PartialTrackSelectionCriterion::OddTracks:
            return track % 2 == 1;
        case PartialTrackSelectionCriterion::OctaveHarmonics:
            return track != 0 && (track & (track - 1)) == 0;
        case PartialTrackSelectionCriterion::FibonacciTracks:
            return isFibonacci(track);
        case PartialTrackSelectionCriterion::PrimeTracks:
            // Track 1 counts among the primes.
            return track == 1 || isPrime(track);
        }
        return false;
    };
    forEachPoint(materialized, [&](const auto& frame, const auto& point, std::size_t) {
        if (chosen(point))
            selection.add(point.trackId, frame.sourceFrame);
    });
    return selection;
}

std::string partialTrackReadout(const talas::project::PartialTrackPoint& point, std::uint64_t frame, double sampleRate)
{
    char text[128];
    const auto seconds = sampleRate > 0.0 ? static_cast<double>(frame) / sampleRate : 0.0;
    if (point.amplitude > 0.0)
    {
        std::snprintf(text, sizeof text, "Track %llu · %.3f s · %.1f dB (%d) · %.1f Hz",
            static_cast<unsigned long long>(point.trackId), seconds, 20.0 * std::log10(point.amplitude),
            partialTrackRawAmplitude(point.amplitude), point.frequencyHz);
    }
    else
    {
        std::snprintf(text, sizeof text, "Track %llu · %.3f s · -inf dB (0) · %.1f Hz",
            static_cast<unsigned long long>(point.trackId), seconds, point.frequencyHz);
    }
    return text;
}

talas::project::PartialTrackAsset partialTrackAssetWithMarkers(
    talas::project::PartialTrackAsset asset, const PartialTrackMarkers& markers)
{
    asset.markers.clear();
    for (const auto& marker : markers.markers)
    {
        if (marker.frame > asset.sourceFrameCount || asset.markers.size() >= talas::project::maximumPartialTrackAssetMarkers)
            continue;
        asset.markers.push_back({marker.name.substr(0, talas::project::maximumPartialTrackMarkerLabelBytes), marker.frame});
    }
    const auto looped = markers.loopStart.has_value() && markers.loopEnd.has_value() && *markers.loopStart < *markers.loopEnd
        && *markers.loopEnd <= asset.sourceFrameCount;
    asset.loopStartFrame = looped ? markers.loopStart : std::nullopt;
    asset.loopEndFrame = looped ? markers.loopEnd : std::nullopt;
    return asset;
}

PartialTrackMarkers partialTrackMarkersOf(const talas::project::PartialTrackAsset& asset)
{
    PartialTrackMarkers markers;
    for (const auto& marker : asset.markers)
        markers.markers.push_back({marker.label, marker.sourceFrame});
    markers.loopStart = asset.loopStartFrame;
    markers.loopEnd = asset.loopEndFrame;
    return markers;
}

std::string defaultPartialTrackMarkerName(const talas::project::PartialTrackAsset& asset, std::uint64_t frame)
{
    std::size_t index = 0;
    for (std::size_t candidate = 0; candidate < asset.frames.size(); ++candidate)
    {
        if (asset.frames[candidate].sourceFrame <= frame)
            index = candidate;
    }
    return "m" + std::to_string(index);
}

std::optional<MaterializedPartialTrackEdits> partialTrackPlayable(
    const MaterializedPartialTrackEdits& materialized,
    PartialTrackPlay play,
    const PartialTrackSelection& selection,
    const PartialTrackMarkers& markers)
{
    switch (play)
    {
    case PartialTrackPlay::All:
        return materialized;
    case PartialTrackPlay::Selection:
    {
        const auto span = selection.frameSpan();
        if (!span.has_value())
            return std::nullopt;
        return retained(
            materialized, [&](std::uint64_t frame, const auto& point) { return selection.contains(point.trackId, frame); },
            [&](std::uint64_t frame) { return frame - span->first; });
    }
    case PartialTrackPlay::UpToStartMarker:
    {
        // Up to the start marker, then stop.
        if (!markers.loopStart.has_value())
            return std::nullopt;
        const auto start = *markers.loopStart;
        return retained(
            materialized, [&](std::uint64_t frame, const auto&) { return frame < start; },
            [](std::uint64_t frame) { return frame; });
    }
    case PartialTrackPlay::BetweenLoopMarkers:
    case PartialTrackPlay::OutsideLoopMarkers:
        break;
    }
    if (!markers.loopStart.has_value() || !markers.loopEnd.has_value() || *markers.loopStart >= *markers.loopEnd)
        return std::nullopt;
    const auto start = *markers.loopStart;
    const auto end = *markers.loopEnd;
    if (play == PartialTrackPlay::OutsideLoopMarkers)
    {
        return retained(
            materialized, [&](std::uint64_t frame, const auto&) { return frame < start || frame >= end; },
            [&](std::uint64_t frame) { return frame >= end ? frame - (end - start) : frame; });
    }
    const auto rate = materialized.asset.sampleRate;
    const auto pre = static_cast<std::uint64_t>(std::llround(std::max(0.0, markers.preRollSeconds) * rate));
    const auto post = static_cast<std::uint64_t>(std::llround(std::max(0.0, markers.postRollSeconds) * rate));
    const auto from = start > pre ? start - pre : 0;
    const auto to = end + post;
    return retained(
        materialized, [&](std::uint64_t frame, const auto&) { return frame >= from && frame <= to; },
        [&](std::uint64_t frame) { return frame - from; });
}

std::optional<MaterializedPartialTrackEdits> partialTrackScrubbed(
    const MaterializedPartialTrackEdits& materialized,
    PartialTrackPlay lastPlay,
    const PartialTrackSelection& selection,
    std::uint64_t frame,
    std::uint64_t frames)
{
    const auto selectedOnly = lastPlay == PartialTrackPlay::Selection;
    const auto tracks = selection.trackIds();
    return retained(
        materialized,
        [&](std::uint64_t at, const auto& point) {
            return at >= frame && at - frame < frames
                && (!selectedOnly || std::ranges::find(tracks, point.trackId) != tracks.end());
        },
        [&](std::uint64_t at) { return at - frame; });
}

std::optional<talas::project::ProjectPartialTrackStateMutation> partialTrackRestoredBySelection(
    std::uint64_t trackId, bool muted, bool solo, std::optional<std::uint64_t> selectedBefore) noexcept
{
    if (!muted || selectedBefore == std::optional<std::uint64_t>{trackId})
        return std::nullopt;
    return talas::project::ProjectPartialTrackStateMutation{trackId, false, solo};
}

talas::project::PartialTrackAsset partialTrackAssetSaved(const MaterializedPartialTrackEdits& materialized)
{
    const auto& states = materialized.trackStates;
    const auto soloed = std::ranges::any_of(states, [](const auto& state) { return state.solo && !state.muted; });
    const auto kept = [&](std::uint64_t trackId) {
        const auto state = std::ranges::find(states, trackId, &talas::project::ProjectPartialTrackState::trackId);
        const auto muted = state != states.end() && state->muted;
        const auto solo = state != states.end() && state->solo && !muted;
        return !muted && (!soloed || solo);
    };
    auto saved = materialized.asset;
    for (auto& frame : saved.frames)
        std::erase_if(frame.points, [&](const talas::project::PartialTrackPoint& point) { return !kept(point.trackId); });
    return saved;
}

std::vector<talas::project::ProjectPartialTrackPointMutation> cutPartialTrackFrames(
    const MaterializedPartialTrackEdits& materialized, std::uint64_t startFrame, std::uint64_t endFrame)
{
    std::vector<talas::project::ProjectPartialTrackPointMutation> mutations;
    if (startFrame >= endFrame)
        return mutations;
    forEachPoint(materialized, [&](const auto& frame, const auto& point, std::size_t flat) {
        if (flat >= materialized.pointIdentities.size() || frame.sourceFrame < startFrame)
            return;
        const auto& identity = materialized.pointIdentities[flat];
        if (frame.sourceFrame < endFrame)
        {
            mutations.push_back({talas::project::ProjectPartialTrackPointMutationKind::Delete, identity});
            return;
        }
        mutations.push_back({
            talas::project::ProjectPartialTrackPointMutationKind::Update,
            identity,
            frame.segmentId,
            frame.sourceFrame - (endFrame - startFrame),
            point,
            flat < materialized.pointInterpolations.size() ? materialized.pointInterpolations[flat]
                                                           : talas::project::PartialTrackPointInterpolation::Linear,
        });
    });
    return mutations;
}

bool partialTrackDotsJoined(bool joinMode, double gapPixels, double dotPixels) noexcept
{
    return joinMode && std::abs(gapPixels) > dotPixels;
}

std::string_view partialTrackAxisLabel(PartialTrackVerticalAxis axis) noexcept
{
    return axis == PartialTrackVerticalAxis::Amplitude ? "AMP" : "FREQ";
}

std::uint32_t partialTrackAxisColour(PartialTrackVerticalAxis axis) noexcept
{
    return axis == PartialTrackVerticalAxis::Amplitude ? 0xffe04040u : 0xff40c060u;
}

PartialTrackEditorViewport partialTrackZoomedIn(PartialTrackEditorViewport viewport)
{
    const auto frames = viewport.endFrame - viewport.startFrame;
    if (frames > 4)
    {
        viewport.startFrame += frames / 4;
        viewport.endFrame = viewport.startFrame + std::max<std::uint64_t>(frames / 2, 2);
    }
    const auto middle = (viewport.minimumFrequencyHz + viewport.maximumFrequencyHz) * 0.5;
    const auto quarter = (viewport.maximumFrequencyHz - viewport.minimumFrequencyHz) * 0.25;
    if (quarter > 0.01)
    {
        viewport.minimumFrequencyHz = middle - quarter;
        viewport.maximumFrequencyHz = middle + quarter;
    }
    return viewport;
}

PartialTrackEditorViewport partialTrackZoomedOut(
    PartialTrackEditorViewport viewport, std::uint64_t sourceFrames, double minimumHz, double maximumHz)
{
    const auto frames = viewport.endFrame - viewport.startFrame;
    const auto start = viewport.startFrame > frames / 2 ? viewport.startFrame - frames / 2 : 0;
    viewport.startFrame = start;
    viewport.endFrame = std::min<std::uint64_t>(std::max<std::uint64_t>(sourceFrames, 1), start + frames * 2);
    const auto middle = (viewport.minimumFrequencyHz + viewport.maximumFrequencyHz) * 0.5;
    const auto span = viewport.maximumFrequencyHz - viewport.minimumFrequencyHz;
    viewport.minimumFrequencyHz = std::max(minimumHz, middle - span);
    viewport.maximumFrequencyHz = std::min(maximumHz, middle + span);
    return viewport;
}

PartialTrackEditorViewport partialTrackZoomedToBox(PartialTrackEditorViewport viewport,
    std::uint64_t startFrame, std::uint64_t endFrame, double lowHz, double highHz)
{
    if (endFrame <= startFrame || !(highHz > lowHz) || !(lowHz > 0.0))
        return viewport;
    viewport.startFrame = startFrame;
    viewport.endFrame = endFrame;
    viewport.minimumFrequencyHz = lowHz;
    viewport.maximumFrequencyHz = highHz;
    return viewport;
}

PartialTrackEditorViewport partialTrackFullDisplay(
    PartialTrackEditorViewport viewport, std::uint64_t sourceFrames, double minimumHz, double maximumHz)
{
    viewport.startFrame = 0;
    viewport.endFrame = std::max<std::uint64_t>(sourceFrames, 1);
    viewport.minimumFrequencyHz = minimumHz;
    viewport.maximumFrequencyHz = maximumHz;
    return viewport;
}

std::uint64_t partialTrackScrubStepped(std::uint64_t frame, std::uint64_t sourceFrames, bool right) noexcept
{
    const auto last = sourceFrames > 0 ? sourceFrames - 1 : 0;
    return right ? std::min(frame + 1, last) : (frame > 0 ? frame - 1 : 0);
}

std::uint64_t partialTrackScrubDragged(std::uint64_t barAtStart, std::int64_t mouseMoved,
    std::uint64_t mouseFrame, bool shift, std::uint64_t sourceFrames) noexcept
{
    const auto last = static_cast<std::int64_t>(sourceFrames > 0 ? sourceFrames - 1 : 0);
    if (shift)
        return static_cast<std::uint64_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(mouseFrame), 0, last));
    return static_cast<std::uint64_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(barAtStart) + mouseMoved, 0, last));
}

std::uint32_t partialTrackScrubBarColour() noexcept { return 0xffe8d44du; }

std::uint32_t partialTrackScrubTimeColour() noexcept { return 0xffffffffu; }

std::string partialTrackScrubTimeText(std::uint64_t frame, double sampleRate)
{
    std::array<char, 32> text{};
    std::snprintf(text.data(), text.size(), "%.3f s", sampleRate > 0.0 ? static_cast<double>(frame) / sampleRate : 0.0);
    return text.data();
}

std::uint64_t partialTrackScrubForPitchBend(int bend, std::uint64_t sourceFrames) noexcept
{
    const auto last = sourceFrames > 0 ? sourceFrames - 1 : 0;
    const auto fraction = static_cast<double>(std::clamp(bend, 0, 16383)) / 16383.0;
    return static_cast<std::uint64_t>(std::llround(fraction * static_cast<double>(last)));
}

std::optional<std::uint64_t> partialTrackAdjacent(
    const std::vector<std::uint64_t>& trackIds, std::optional<std::uint64_t> current, bool up)
{
    if (trackIds.empty())
        return std::nullopt;
    auto sorted = trackIds;
    std::ranges::sort(sorted);
    if (!current.has_value())
        return up ? sorted.front() : sorted.back();
    if (up)
    {
        const auto above = std::ranges::upper_bound(sorted, *current);
        return above == sorted.end() ? sorted.back() : *above;
    }
    const auto below = std::ranges::lower_bound(sorted, *current);
    return below == sorted.begin() ? sorted.front() : *std::prev(below);
}

std::optional<talas::project::PartialTrackAsset> partialTrackRegionAsset(
    const talas::project::PartialTrackAsset& asset, const PartialTrackSelection& selection)
{
    const auto span = selection.frameSpan();
    if (!span.has_value())
        return std::nullopt;
    talas::project::PartialTrackAsset region;
    region.sampleRate = asset.sampleRate;
    region.sourceFrameCount = span->second - span->first + 1;
    for (const auto& frame : asset.frames)
    {
        if (frame.sourceFrame < span->first || frame.sourceFrame > span->second)
            continue;
        talas::project::PartialTrackFrame copied{frame.segmentId, frame.sourceFrame - span->first, {}};
        for (const auto& point : frame.points)
            if (selection.contains(point.trackId, frame.sourceFrame))
                copied.points.push_back(point);
        region.frames.push_back(std::move(copied));
    }
    return region;
}

double partialTrackDrawnValue(PartialTrackVerticalAxis axis, double heightRatio, double lowestHz, double highestHz) noexcept
{
    const auto ratio = std::clamp(heightRatio, 0.0, 1.0);
    if (axis == PartialTrackVerticalAxis::Amplitude)
        return 1.0 - ratio;
    return highestHz + (lowestHz - highestHz) * ratio;
}

std::uint32_t partialTrackAmplitudeColour(double amplitude, bool selected) noexcept
{
    const auto decibels = 20.0 * std::log10(std::max(amplitude, 1.0e-6));
    const auto level = std::clamp((decibels + 60.0) / 60.0, 0.0, 1.0);
    struct Stop
    {
        double r;
        double g;
        double b;
    };
    constexpr Stop quiet{20.0, 30.0, 90.0};
    constexpr Stop middle{40.0, 170.0, 80.0};
    constexpr Stop loud{240.0, 230.0, 60.0};
    const auto [from, to, t] = level < 0.5 ? std::tuple{quiet, middle, level * 2.0} : std::tuple{middle, loud, (level - 0.5) * 2.0};
    auto r = from.r + (to.r - from.r) * t;
    auto g = from.g + (to.g - from.g) * t;
    auto b = from.b + (to.b - from.b) * t;
    if (selected)
    {
        r += (255.0 - r) * 0.45;
        g += (255.0 - g) * 0.45;
        b += (255.0 - b) * 0.45;
    }
    const auto channel = [](double value) { return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 255.0))); };
    return 0xff000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

std::uint32_t partialTrackClearedColour() noexcept { return 0xff505050u; }

std::uint32_t spectrumEditorBackgroundColour() noexcept { return 0xff000000u; }

std::uint32_t spectrumEditorLoopMarkerColour() noexcept { return 0xffe0443cu; }

std::string cutPartialTrackFramesQuestion(std::uint64_t startFrame, std::uint64_t endFrame)
{
    return "Delete the frames from " + std::to_string(startFrame) + " to "
        + std::to_string(endFrame > startFrame ? endFrame - 1 : startFrame) + "?";
}

std::span<const SpectrumEditorButtonInfo> spectrumEditorButtons() noexcept
{
    static constexpr std::array<SpectrumEditorButtonInfo, 12> buttons{{
        {SpectrumEditorButton::PlayAll, 1, "Play"},
        {SpectrumEditorButton::PlaySelection, 2, "Play sel."},
        {SpectrumEditorButton::PlayBetweenLoopMarkers, 3, "Play loop"},
        {SpectrumEditorButton::PlayOutsideLoopMarkers, 4, "Play rest"},
        {SpectrumEditorButton::Marker, 5, "Marker"},
        {SpectrumEditorButton::LoopStart, 6, "Start >"},
        {SpectrumEditorButton::LoopEnd, 7, "< End"},
        {SpectrumEditorButton::PrePostRoll, 8, "Roll..."},
        {SpectrumEditorButton::SelectionCriteria, 9, "Select by"},
        {SpectrumEditorButton::TrackFilters, 10, "Filters"},
        {SpectrumEditorButton::Cut, 11, "Cut"},
        {SpectrumEditorButton::Draw, 12, "Draw"},
    }};
    return buttons;
}

std::optional<SpectrumEditorButton> spectrumEditorButtonForFunctionKey(int functionKey) noexcept
{
    for (const auto& info : spectrumEditorButtons())
    {
        if (info.functionKey == functionKey)
            return info.button;
    }
    return std::nullopt;
}

PartialTrackPlay spectrumEditorFourthButtonPlay(bool upToStartMarker) noexcept
{
    return upToStartMarker ? PartialTrackPlay::UpToStartMarker : PartialTrackPlay::OutsideLoopMarkers;
}

std::string_view spectrumEditorFourthButtonLabel(bool upToStartMarker) noexcept
{
    return upToStartMarker ? "Play to start" : "Play rest";
}
}
