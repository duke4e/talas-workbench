#include "talas/audio/PartialTrackDerivative.h"

#include "talas/audio/AudioFilePublication.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::audio
{
namespace
{
constexpr std::size_t partialWriteChunkBytes = 64U * 1024U;

[[nodiscard]] PartialTrackDerivativeResult failure(
    PartialTrackDerivativeStatus status,
    const PartialTrackDerivativeRequest& request,
    std::string message)
{
    return {
        status,
        request.destination,
        {},
        0.0,
        0,
        {},
        std::move(message),
    };
}

[[nodiscard]] bool cancelled(
    const PartialTrackDerivativeCancellationCheck& check) noexcept
{
    if (!check)
        return false;
    try
    {
        return check();
    }
    catch (...)
    {
        return true;
    }
}

[[nodiscard]] bool authorityCurrent(
    const PartialTrackDerivativeAuthorityCheck& check) noexcept
{
    if (!check)
        return true;
    try
    {
        return check();
    }
    catch (...)
    {
        return false;
    }
}

void progress(
    const PartialTrackDerivativeProgress& callback,
    std::uint16_t value) noexcept
{
    if (!callback)
        return;
    try
    {
        callback(std::min<std::uint16_t>(value, 1000));
    }
    catch (...)
    {
    }
}

[[nodiscard]] bool sourceAuthorityIsValid(
    const PartialTrackDerivativeRequest& request) noexcept
{
    if (request.destination.empty()
        || request.source == nullptr
        || request.blockFrames == 0
        || request.blockFrames > (1U << 20U))
    {
        return false;
    }
    const auto& manifest = request.sourceManifest;
    const auto& source = *request.source;
    if (manifest.kind
        != talas::project::ProjectAssetKind::PartialSpectrum
        || manifest.id.empty()
        || manifest.path.empty()
        || manifest.generation == 0
        || manifest.generation
            > talas::project::maximumProjectAssetJsonInteger
        || !talas::project::isCanonicalProjectAssetContentHash(
            manifest.contentHash)
        || manifest.channelCount != 0
        || manifest.encoding
            != talas::project::partialTrackAssetEncoding
        || manifest.sampleRate != source.sampleRate
        || manifest.frameCount != source.sourceFrameCount
        || talas::project::validatePartialTrackAsset(source).has_value())
    {
        return false;
    }
    return !request.edits.has_value()
        || talas::project::partialTrackEditSetMatchesAsset(
            *request.edits,
            manifest);
}

[[nodiscard]] talas::project::MaterializedPartialTrackEditsResult
materialize(const PartialTrackDerivativeRequest& request)
{
    if (request.edits.has_value())
    {
        return talas::project::materializeProjectPartialTrackEdits(
            *request.source,
            *request.edits);
    }
    talas::project::MaterializedPartialTrackEditsResult result;
    result.materialized.asset = *request.source;
    std::size_t pointCount = 0;
    for (const auto& frame : request.source->frames)
        pointCount += frame.points.size();
    result.materialized.pointInterpolations.assign(
        pointCount,
        talas::project::PartialTrackPointInterpolation::Linear);
    return result;
}

void applyTrackStates(
    talas::project::MaterializedPartialTrackEdits& materialized)
{
    const auto hasSolo = std::ranges::any_of(
        materialized.trackStates,
        &talas::project::ProjectPartialTrackState::solo);
    std::unordered_set<std::uint64_t> mutedTrackIds;
    std::unordered_set<std::uint64_t> soloTrackIds;
    mutedTrackIds.reserve(materialized.trackStates.size());
    soloTrackIds.reserve(materialized.trackStates.size());
    for (const auto& state : materialized.trackStates)
    {
        if (state.muted)
            mutedTrackIds.insert(state.trackId);
        if (state.solo && !state.muted)
            soloTrackIds.insert(state.trackId);
    }
    if (!hasSolo && mutedTrackIds.empty())
        return;

    std::vector<talas::project::PartialTrackPointInterpolation> retained;
    retained.reserve(materialized.pointInterpolations.size());
    std::size_t interpolationIndex = 0;
    for (auto& frame : materialized.asset.frames)
    {
        auto output = frame.points.begin();
        for (const auto& point : frame.points)
        {
            if (!mutedTrackIds.contains(point.trackId)
                && (!hasSolo || soloTrackIds.contains(point.trackId)))
            {
                retained.push_back(
                    materialized.pointInterpolations[interpolationIndex]);
                *output++ = point;
            }
            ++interpolationIndex;
        }
        frame.points.erase(output, frame.points.end());
    }
    materialized.pointInterpolations = std::move(retained);
}

[[nodiscard]] std::uint16_t scaledProgress(
    std::uint64_t completed,
    std::uint64_t total,
    std::uint16_t begin,
    std::uint16_t end) noexcept
{
    if (total == 0 || completed >= total)
        return end;
    const auto range = static_cast<std::uint64_t>(end - begin);
    return static_cast<std::uint16_t>(
        begin + completed * range / total);
}
}

PartialTrackDerivativeResult renderPartialTrackDerivative(
    const PartialTrackDerivativeRequest& request,
    PartialTrackDerivativeCancellationCheck shouldCancel,
    PartialTrackDerivativeAuthorityCheck terminalAuthorityCheck,
    PartialTrackDerivativeProgress reportProgress)
{
    progress(reportProgress, 0);
    if (!sourceAuthorityIsValid(request))
    {
        return failure(
            PartialTrackDerivativeStatus::InvalidRequest,
            request,
            "Partial-track derivative request does not name one exact immutable source generation");
    }
    if (!request.resolvedSourcePath.empty()
        && audioFileDestinationConflicts(
            request.destination,
            std::span<const std::filesystem::path>{
                &request.resolvedSourcePath, 1},
            {}))
    {
        return failure(
            PartialTrackDerivativeStatus::DestinationRejected,
            request,
            "Partial-track derivative destination aliases the source asset");
    }
    if (cancelled(shouldCancel))
    {
        return failure(
            PartialTrackDerivativeStatus::Cancelled,
            request,
            "Partial-track derivative cancelled before preparation");
    }

    try
    {
        const auto canonicalSource =
            talas::project::encodePartialTrackAsset(*request.source);
        if (!canonicalSource.ok()
            || talas::project::calculateProjectAssetSha256(
                    canonicalSource.bytes)
                != request.sourceManifest.contentHash)
        {
            return failure(
                PartialTrackDerivativeStatus::InvalidRequest,
                request,
                "Partial-track derivative source bytes do not match the exact manifest hash");
        }
        auto materialized = materialize(request);
        if (!materialized.ok())
        {
            return failure(
                PartialTrackDerivativeStatus::PreparationFailed,
                request,
                materialized.error.has_value()
                    ? materialized.error->message
                    : "Partial-track edits could not be materialized");
        }
        applyTrackStates(materialized.materialized);
        progress(reportProgress, 100);

        std::string publicationError;
        auto publication = AudioFilePublication::prepare(
            request.destination,
            ExistingAudioFilePolicy::Reject,
            publicationError);
        if (publication == nullptr)
        {
            return failure(
                PartialTrackDerivativeStatus::DestinationRejected,
                request,
                std::move(publicationError));
        }

        PartialTrackDerivativeResult result;
        result.status = PartialTrackDerivativeStatus::Succeeded;
        result.destination = request.destination;
        result.sampleRate = materialized.materialized.asset.sampleRate;
        result.frameCount = materialized.materialized.asset.sourceFrameCount;
        if (request.kind == PartialTrackDerivativeKind::EditedPartial)
        {
            const auto encoded = talas::project::encodePartialTrackAsset(
                materialized.materialized.asset);
            if (!encoded.ok())
            {
                return failure(
                    PartialTrackDerivativeStatus::PreparationFailed,
                    request,
                    encoded.error.has_value()
                        ? encoded.error->message
                        : "Edited partial-track derivative encoding failed");
            }
            std::ofstream output(
                publication->temporaryPath(),
                std::ios::binary | std::ios::trunc);
            if (!output.good())
            {
                return failure(
                    PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    "Unable to open private partial-track derivative staging file");
            }
            std::size_t offset = 0;
            while (offset < encoded.bytes.size())
            {
                if (cancelled(shouldCancel))
                {
                    return failure(
                        PartialTrackDerivativeStatus::Cancelled,
                        request,
                        "Edited partial-track derivative cancelled");
                }
                const auto count = std::min(
                    partialWriteChunkBytes,
                    encoded.bytes.size() - offset);
                output.write(
                    reinterpret_cast<const char*>(
                        encoded.bytes.data() + offset),
                    static_cast<std::streamsize>(count));
                if (!output.good())
                {
                    return failure(
                        PartialTrackDerivativeStatus::RenderFailed,
                        request,
                        "Unable to write private partial-track derivative staging file");
                }
                offset += count;
                progress(
                    reportProgress,
                    scaledProgress(
                        offset, encoded.bytes.size(), 100, 800));
            }
            output.flush();
            output.close();
            if (output.fail())
            {
                return failure(
                    PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    "Unable to finalize private partial-track derivative staging file");
            }
            const auto hash = talas::project::calculateProjectAssetSha256(
                publication->temporaryPath(),
                shouldCancel);
            if (!hash.ok())
            {
                return failure(
                    hash.error->code
                            == talas::project::ProjectAssetInspectionErrorCode::Cancelled
                        ? PartialTrackDerivativeStatus::Cancelled
                        : PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    hash.error->message);
            }
            const auto expectedHash =
                talas::project::calculateProjectAssetSha256(encoded.bytes);
            if (hash.contentHash != expectedHash)
            {
                return failure(
                    PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    "Edited partial-track staging bytes changed before publication");
            }
            result.contentHash = hash.contentHash;
            result.encoding = std::string{
                talas::project::partialTrackAssetEncoding};
        }
        else if (request.kind == PartialTrackDerivativeKind::Audio)
        {
            PreparedPartialTrackResynthesizer synthesizer{
                materialized.materialized.asset,
                materialized.materialized.pointInterpolations};
            if (!synthesizer.ready()
                || synthesizer.sourceFrameCount()
                    > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max()))
            {
                return failure(
                    PartialTrackDerivativeStatus::PreparationFailed,
                    request,
                    "Edited partial-track resynthesizer could not be prepared");
            }
            Pcm16WavStreamWriter writer(
                publication->temporaryPath(),
                synthesizer.sampleRate(),
                1,
                static_cast<std::size_t>(
                    synthesizer.sourceFrameCount()));
            if (!writer.ready())
            {
                return failure(
                    PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    std::string{writer.error()});
            }
            std::vector<double> block(request.blockFrames);
            std::array<std::vector<double>, 1> channels;
            std::uint64_t offset = 0;
            while (offset < synthesizer.sourceFrameCount())
            {
                if (cancelled(shouldCancel))
                {
                    writer.abandon();
                    return failure(
                        PartialTrackDerivativeStatus::Cancelled,
                        request,
                        "Partial-track audio derivative cancelled");
                }
                const auto count = static_cast<std::size_t>(std::min<
                    std::uint64_t>(
                        block.size(),
                        synthesizer.sourceFrameCount() - offset));
                if (synthesizer.render(
                        offset,
                        std::span<double>{block}.first(count))
                    != PartialTrackResynthesisStatus::Ok)
                {
                    writer.abandon();
                    return failure(
                        PartialTrackDerivativeStatus::RenderFailed,
                        request,
                        "Prepared partial-track audio derivative render failed");
                }
                channels[0].assign(block.begin(), block.begin() + count);
                if (!writer.write(channels))
                {
                    return failure(
                        PartialTrackDerivativeStatus::RenderFailed,
                        request,
                        std::string{writer.error()});
                }
                offset += count;
                progress(
                    reportProgress,
                    scaledProgress(
                        offset,
                        synthesizer.sourceFrameCount(),
                        100,
                        800));
            }
            if (!writer.finish())
            {
                return failure(
                    PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    std::string{writer.error()});
            }
            const auto hash = talas::project::calculateProjectAssetSha256(
                publication->temporaryPath(),
                shouldCancel);
            if (!hash.ok())
            {
                return failure(
                    hash.error->code
                            == talas::project::ProjectAssetInspectionErrorCode::Cancelled
                        ? PartialTrackDerivativeStatus::Cancelled
                        : PartialTrackDerivativeStatus::RenderFailed,
                    request,
                    hash.error->message);
            }
            result.contentHash = hash.contentHash;
            result.encoding = std::string{
                talas::project::pcm16WavAssetEncoding};
        }
        else
        {
            return failure(
                PartialTrackDerivativeStatus::InvalidRequest,
                request,
                "Partial-track derivative kind is unsupported");
        }

        progress(reportProgress, 900);
        if (cancelled(shouldCancel))
        {
            return failure(
                PartialTrackDerivativeStatus::Cancelled,
                request,
                "Partial-track derivative cancelled before publication");
        }
        if (!authorityCurrent(terminalAuthorityCheck))
        {
            return failure(
                PartialTrackDerivativeStatus::AuthorityChanged,
                request,
                "Partial-track source or edit authority changed before publication");
        }
        if (!publication->publish(publicationError))
        {
            return failure(
                PartialTrackDerivativeStatus::PublicationFailed,
                request,
                std::move(publicationError));
        }
        progress(reportProgress, 1000);
        return result;
    }
    catch (const std::bad_alloc&)
    {
        return failure(
            PartialTrackDerivativeStatus::AllocationFailed,
            request,
            "Partial-track derivative allocation failed");
    }
    catch (const std::exception& exception)
    {
        return failure(
            PartialTrackDerivativeStatus::RenderFailed,
            request,
            std::string{"Partial-track derivative failed: "}
                + exception.what());
    }
    catch (...)
    {
        return failure(
            PartialTrackDerivativeStatus::RenderFailed,
            request,
            "Partial-track derivative failed");
    }
}
}
