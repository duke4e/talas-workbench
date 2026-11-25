#include "talas/runtime/SampleMapPlayback.h"

#include "talas/project/ProjectDocument.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace talas::runtime
{
namespace
{
[[nodiscard]] SampleMapPlaybackPreparationResult fail(
    std::string path,
    std::string message)
{
    return {
        std::nullopt,
        std::move(path),
        std::move(message),
    };
}

[[nodiscard]] const talas::project::ProjectAsset* findAsset(
    const talas::project::ProjectDocument& document,
    std::string_view assetId) noexcept
{
    const auto found = std::ranges::find(
        document.assets,
        assetId,
        &talas::project::ProjectAsset::id);
    return found == document.assets.end() ? nullptr : &*found;
}

[[nodiscard]] const talas::core::ResourceSnapshot* findSnapshot(
    std::span<const talas::core::ResourceSnapshotPtr> snapshots,
    std::string_view resourceId) noexcept
{
    const auto found = std::ranges::find_if(
        snapshots,
        [&](const auto& snapshot) {
            return snapshot != nullptr && snapshot->id == resourceId;
        });
    return found == snapshots.end() ? nullptr : found->get();
}

[[nodiscard]] bool snapshotMatchesAsset(
    const talas::core::ResourceSnapshot& snapshot,
    const talas::project::ProjectAsset& asset) noexcept
{
    return snapshot.kind == talas::core::ResourceSnapshotKind::Audio
        && snapshot.generation == asset.generation
        && snapshot.contentHash == asset.contentHash
        && snapshot.channelCount == asset.channelCount
        && snapshot.sampleRate == asset.sampleRate
        && snapshot.frameCount == asset.frameCount
        && snapshot.encoding == asset.encoding;
}

[[nodiscard]] std::optional<std::uint8_t> midiNoteForFrequency(
    double frequency) noexcept
{
    if (!std::isfinite(frequency) || frequency <= 0.0)
        return std::nullopt;
    const auto note = std::lround(
        69.0 + 12.0 * std::log2(frequency / 440.0));
    if (note < 0 || note > 127)
        return std::nullopt;
    return static_cast<std::uint8_t>(note);
}

[[nodiscard]] std::optional<std::uint8_t> midiVelocity(
    double velocity) noexcept
{
    if (!std::isfinite(velocity)
        || velocity <= 0.0
        || velocity > 1.0)
    {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(std::clamp<long>(
        std::lround(velocity * 127.0),
        1,
        127));
}

[[nodiscard]] std::uint64_t boundedEnvelopeFrames(
    double seconds,
    double sampleRate) noexcept
{
    if (!std::isfinite(seconds)
        || seconds <= 0.0
        || !std::isfinite(sampleRate)
        || sampleRate <= 0.0)
    {
        return 0;
    }
    const auto frames = seconds * sampleRate;
    if (!std::isfinite(frames)
        || frames >= static_cast<double>(
            std::numeric_limits<std::uint64_t>::max()))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return std::max<std::uint64_t>(
        1,
        static_cast<std::uint64_t>(std::ceil(frames)));
}

[[nodiscard]] const talas::project::ProjectSampleLoop* activeLoop(
    const PreparedSampleMapRegion& region,
    const SampleMapPlaybackLayerState& layer) noexcept
{
    if (layer.released)
    {
        return region.releaseLoop.has_value()
            ? &*region.releaseLoop
            : nullptr;
    }
    return region.sustainLoop.has_value()
        ? &*region.sustainLoop
        : nullptr;
}

void normalizeForwardLoop(
    SampleMapPlaybackLayerState& layer,
    const talas::project::ProjectSampleLoop& loop) noexcept
{
    const auto start = static_cast<double>(loop.startFrame);
    const auto end = static_cast<double>(loop.endFrameExclusive);
    const auto length = end - start;
    if (length <= 1.0)
    {
        layer.position = start;
        return;
    }
    auto phase = std::fmod(layer.position - start, length);
    if (phase < 0.0)
        phase += length;
    layer.position = start + phase;
}

void normalizeAlternateLoop(
    SampleMapPlaybackLayerState& layer,
    const talas::project::ProjectSampleLoop& loop) noexcept
{
    const auto start = static_cast<double>(loop.startFrame);
    const auto last = static_cast<double>(
        loop.endFrameExclusive - 1U);
    const auto distance = last - start;
    if (distance <= 0.0)
    {
        layer.position = start;
        layer.direction = layer.direction < 0.0 ? -1.0 : 1.0;
        return;
    }

    const auto period = 2.0 * distance;
    auto unfolded = layer.direction >= 0.0
        ? layer.position - start
        : period - (layer.position - start);
    auto phase = std::fmod(unfolded, period);
    if (phase < 0.0)
        phase += period;
    if (phase < distance)
    {
        layer.position = start + phase;
        layer.direction = 1.0;
    }
    else
    {
        layer.position = start + (period - phase);
        layer.direction = -1.0;
    }
}

void normalizeLoop(
    SampleMapPlaybackLayerState& layer,
    const talas::project::ProjectSampleLoop& loop) noexcept
{
    if (loop.mode == talas::project::ProjectSampleLoopMode::Alternate)
        normalizeAlternateLoop(layer, loop);
    else
        normalizeForwardLoop(layer, loop);
}

void advanceLayer(
    SampleMapPlaybackLayerState& layer,
    const talas::project::ProjectSampleLoop* loop,
    std::size_t sourceFrameCount) noexcept
{
    layer.position += layer.direction * layer.increment;
    if (loop != nullptr)
    {
        const auto start = static_cast<double>(loop->startFrame);
        const auto upperBoundary = loop->mode
                == talas::project::ProjectSampleLoopMode::Alternate
            ? static_cast<double>(
                  loop->endFrameExclusive - 1U)
            : static_cast<double>(
                  loop->endFrameExclusive);
        const auto crossedBoundary = layer.direction >= 0.0
            ? layer.position > upperBoundary
                || (loop->mode
                        == talas::project::ProjectSampleLoopMode::Forward
                    && layer.position == upperBoundary)
            : layer.position < start;
        if (crossedBoundary)
            normalizeLoop(layer, *loop);
        return;
    }
    if (layer.position < 0.0
        || layer.position
            > static_cast<double>(sourceFrameCount - 1U))
    {
        layer.active = false;
    }
}

[[nodiscard]] double readSource(
    const SampleMapPlaybackSource& source,
    double position,
    double nextPosition,
    double signedIncrement,
    const talas::project::ProjectSampleLoop* loop) noexcept
{
    if ((source.samples == nullptr && source.read == nullptr)
        || source.frameCount == 0
        || !std::isfinite(position)
        || position < 0.0
        || position > static_cast<double>(source.frameCount - 1U))
    {
        return 0.0;
    }
    const auto left = static_cast<std::size_t>(std::floor(position));
    auto right = std::min(left + 1U, source.frameCount - 1U);
    if (loop != nullptr
        && loop->mode == talas::project::ProjectSampleLoopMode::Forward
        && position >= static_cast<double>(loop->startFrame)
        && position
            < static_cast<double>(loop->endFrameExclusive)
        && left + 1U >= loop->endFrameExclusive)
    {
        right = static_cast<std::size_t>(loop->startFrame);
    }
    const auto fraction = position - static_cast<double>(left);
    if (source.read != nullptr)
    {
        const auto streamed = source.read(
            source.context,
            position,
            static_cast<double>(right),
            nextPosition,
            signedIncrement);
        return streamed.available ? streamed.sample : 0.0;
    }
    return source.samples[left]
        + (source.samples[right] - source.samples[left]) * fraction;
}

[[nodiscard]] bool startRelease(
    const PreparedSampleMapRegion& region,
    SampleMapPlaybackLayerState& layer,
    double releaseSeconds,
    double outputSampleRate) noexcept
{
    layer.released = true;
    layer.envelopeFramesElapsed = 0;
    layer.releaseStartEnvelope = layer.envelope;
    layer.releaseFrames = boundedEnvelopeFrames(
        releaseSeconds,
        outputSampleRate);
    if (layer.releaseFrames == 0)
    {
        layer.active = false;
        return false;
    }
    if (!region.releaseLoop.has_value())
        return false;

    layer.direction = region.reverse ? -1.0 : 1.0;
    layer.position = region.reverse
        ? static_cast<double>(
              region.releaseLoop->endFrameExclusive - 1U)
        : static_cast<double>(region.releaseLoop->startFrame);
    return true;
}

void triggerLayers(
    const PreparedSampleMapPlayback& playback,
    SampleMapPlaybackState& state,
    std::span<const SampleMapPlaybackSource> sources,
    std::uint8_t note,
    std::uint8_t velocity,
    double attackSeconds,
    double outputSampleRate) noexcept
{
    state.layerCount = 0;
    for (std::size_t regionIndex = 0;
         regionIndex < playback.regions.size()
             && state.layerCount < state.layers.size();
         ++regionIndex)
    {
        const auto& region = playback.regions[regionIndex];
        if (note < region.lowNote
            || note > region.highNote
            || velocity < region.lowVelocity
            || velocity > region.highVelocity
            || regionIndex >= sources.size())
        {
            continue;
        }
        const auto& source = sources[regionIndex];
        if ((source.samples == nullptr && source.read == nullptr)
            || source.frameCount == 0
            || !std::isfinite(source.sampleRate)
            || source.sampleRate <= 0.0)
        {
            continue;
        }

        auto& layer = state.layers[state.layerCount++];
        layer = {};
        layer.regionIndex = regionIndex;
        layer.direction = region.reverse ? -1.0 : 1.0;
        layer.position = region.reverse
            ? static_cast<double>(source.frameCount - 1U)
            : 0.0;
        layer.increment = std::pow(
            2.0,
            (static_cast<double>(note)
             - static_cast<double>(region.rootNote))
                / 12.0)
            * source.sampleRate / outputSampleRate;
        layer.attackFrames = boundedEnvelopeFrames(
            attackSeconds,
            outputSampleRate);
        layer.envelopeFramesElapsed = 0;
        layer.envelope = layer.attackFrames == 0 ? 1.0 : 0.0;
        layer.active = std::isfinite(layer.increment)
            && layer.increment > 0.0;
        if (layer.active
            && source.restart != nullptr
            && !source.restart(
                source.context,
                layer.position,
                layer.direction * layer.increment))
        {
            layer.active = false;
        }
    }
}
}

SampleMapPlaybackPreparationResult prepareSampleMapPlayback(
    const talas::project::ProjectDocument& document,
    std::string_view mapId,
    std::span<const talas::core::ResourceSnapshotPtr> resourceSnapshots,
    std::string_view structuralSourcePath)
{
    if (mapId.empty())
    {
        return fail(
            std::string(structuralSourcePath),
            "Sample Map Player requires a non-empty sampleMapId");
    }
    const auto* map = talas::project::findProjectSampleMap(document, mapId);
    if (map == nullptr)
    {
        return fail(
            std::string(structuralSourcePath),
            "Sample Map Player cannot resolve sample map '"
                + std::string(mapId) + "'");
    }

    const auto mapPath = std::string(structuralSourcePath)
        + "/sampleMap[" + map->mapId + "]";
    const auto diagnostics =
        talas::project::projectSampleMapDiagnostics(document);
    if (!diagnostics.empty())
    {
        return fail(
            mapPath,
            "Sample Map Player preparation rejected invalid project map state at "
                + diagnostics.front().path + ": "
                + diagnostics.front().message);
    }

    PreparedSampleMapPlayback prepared;
    prepared.mapId = map->mapId;
    prepared.regions.reserve(map->regions.size());
    for (const auto& region : map->regions)
    {
        const auto regionPath = mapPath
            + "/region[" + region.regionId + "]";
        const auto* asset = findAsset(document, region.assetId);
        if (asset == nullptr
            || asset->kind != talas::project::ProjectAssetKind::Audio)
        {
            return fail(
                regionPath,
                "Sample-map region '" + region.regionId
                    + "' does not resolve an Audio asset '"
                    + region.assetId + "'");
        }
        const auto* snapshot = findSnapshot(
            resourceSnapshots,
            region.assetId);
        if (snapshot == nullptr)
        {
            return fail(
                regionPath,
                "Sample-map region '" + region.regionId
                    + "' has no prepared immutable Audio resource '"
                    + region.assetId + "'");
        }
        if (!snapshotMatchesAsset(*snapshot, *asset))
        {
            return fail(
                regionPath,
                "Sample-map region '" + region.regionId
                    + "' Audio resource identity or media shape does not match asset '"
                    + region.assetId + "'");
        }
        if (!std::isfinite(snapshot->sampleRate)
            || snapshot->sampleRate <= 0.0
            || snapshot->frameCount == 0)
        {
            return fail(
                regionPath,
                "Sample-map region '" + region.regionId
                    + "' requires positive prepared Audio media bounds");
        }

        prepared.regions.push_back({
            region.regionId,
            region.assetId,
            regionPath,
            region.rootNote,
            region.lowNote,
            region.highNote,
            region.lowVelocity,
            region.highVelocity,
            region.reverse,
            region.sustainLoop,
            region.releaseLoop,
            snapshot->sampleRate,
            snapshot->frameCount,
        });
    }
    return {std::move(prepared), {}, {}};
}

void SampleMapPlaybackState::reset() noexcept
{
    layers = {};
    layerCount = 0;
    gateWasOpen = false;
}

double renderSampleMapPlaybackFrame(
    const PreparedSampleMapPlayback& playback,
    SampleMapPlaybackState& state,
    std::span<const SampleMapPlaybackSource> sources,
    const SampleMapPlaybackControls& controls,
    double outputSampleRate) noexcept
{
    const auto note = midiNoteForFrequency(controls.frequency);
    const auto velocity = midiVelocity(controls.velocity);
    const auto gateOpen = std::isfinite(controls.gate)
        && controls.gate > 0.0
        && note.has_value()
        && velocity.has_value();

    if (gateOpen && !state.gateWasOpen)
    {
        triggerLayers(
            playback,
            state,
            sources,
            *note,
            *velocity,
            controls.attackSeconds,
            outputSampleRate);
    }
    else if (!gateOpen && state.gateWasOpen)
    {
        for (std::size_t index = 0;
             index < state.layerCount;
             ++index)
        {
            auto& layer = state.layers[index];
            if (!layer.active
                || layer.regionIndex >= playback.regions.size())
            {
                continue;
            }
            const auto& region =
                playback.regions[layer.regionIndex];
            const auto relocated = startRelease(
                region,
                layer,
                controls.releaseSeconds,
                outputSampleRate);
            if (relocated)
            {
                if (layer.regionIndex >= sources.size())
                {
                    layer.active = false;
                    continue;
                }
                const auto& source = sources[layer.regionIndex];
                if (source.read != nullptr
                    && (source.relocate == nullptr
                        || !source.relocate(
                            source.context,
                            layer.position,
                            layer.direction * layer.increment)))
                {
                    layer.active = false;
                }
            }
        }
    }
    state.gateWasOpen = gateOpen;

    auto mixed = 0.0;
    for (std::size_t index = 0;
         index < state.layerCount;
         ++index)
    {
        auto& layer = state.layers[index];
        if (!layer.active
            || layer.regionIndex >= playback.regions.size()
            || layer.regionIndex >= sources.size())
        {
            continue;
        }
        const auto& region = playback.regions[layer.regionIndex];
        const auto& source = sources[layer.regionIndex];
        const auto* loop = activeLoop(region, layer);

        auto advanced = layer;
        advanceLayer(
            advanced,
            loop,
            source.frameCount);
        mixed += readSource(
                     source,
                     layer.position,
                     advanced.position,
                     advanced.direction * advanced.increment,
                     loop)
            * layer.envelope;

        layer.position = advanced.position;
        layer.direction = advanced.direction;
        layer.active = advanced.active;

        if (layer.released)
        {
            ++layer.envelopeFramesElapsed;
            if (layer.envelopeFramesElapsed >= layer.releaseFrames)
            {
                layer.envelope = 0.0;
                layer.active = false;
            }
            else
            {
                layer.envelope = layer.releaseStartEnvelope
                    * (1.0
                       - static_cast<double>(
                             layer.envelopeFramesElapsed)
                           / static_cast<double>(
                               layer.releaseFrames));
            }
        }
        else if (layer.attackFrames > 0
                 && layer.envelope < 1.0)
        {
            ++layer.envelopeFramesElapsed;
            layer.envelope = std::min(
                1.0,
                static_cast<double>(layer.envelopeFramesElapsed)
                    / static_cast<double>(layer.attackFrames));
        }
    }

    return std::isfinite(controls.scale)
        ? mixed * controls.scale
        : 0.0;
}
}
