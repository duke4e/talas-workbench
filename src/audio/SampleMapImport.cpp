#include "talas/audio/SampleMapImport.h"

#include "talas/audio/ProjectPackager.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectSampleMaps.h"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <span>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace talas::audio
{
namespace
{
struct InspectedSource
{
    std::filesystem::path path;
    std::string basename;
    std::string normalizedPath;
    talas::project::ProjectAsset asset;
};

struct NewAssetSource
{
    std::string assetId;
    std::filesystem::path sourcePath;
};

[[nodiscard]] bool cancellationRequested(
    const SampleMapImportOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

void reportProgress(
    const SampleMapImportOptions& options,
    double progress)
{
    if (options.reportProgress)
        options.reportProgress(std::clamp(progress, 0.0, 1.0));
}

[[nodiscard]] std::string asciiCaseFold(std::string_view value)
{
    std::string folded;
    folded.reserve(value.size());
    for (const auto character : value)
    {
        folded.push_back(
            character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character);
    }
    return folded;
}

[[nodiscard]] std::string identifierToken(
    const std::filesystem::path& path)
{
    const auto source = asciiCaseFold(
        talas::project::pathToUtf8(path.stem()));
    std::string token;
    token.reserve(std::min<std::size_t>(source.size(), 72));
    auto previousWasSeparator = false;
    for (const auto byte : source)
    {
        const auto character = static_cast<unsigned char>(byte);
        if (std::isalnum(character) != 0)
        {
            token.push_back(static_cast<char>(character));
            previousWasSeparator = false;
        }
        else if (!previousWasSeparator)
        {
            token.push_back('_');
            previousWasSeparator = true;
        }
        if (token.size() == 72)
            break;
    }
    while (!token.empty() && token.front() == '_')
        token.erase(token.begin());
    while (!token.empty() && token.back() == '_')
        token.pop_back();
    return token.empty() ? "sample" : token;
}

[[nodiscard]] std::string uniqueAssetId(
    std::string_view token,
    std::span<const talas::project::ProjectAsset> assets)
{
    const auto base = "Samples/" + std::string(token);
    auto candidate = base;
    for (std::size_t ordinal = 2;
         std::ranges::any_of(
             assets,
             [&](const auto& asset) {
                 return asset.id == candidate;
             });
         ++ordinal)
    {
        candidate = base + "#" + std::to_string(ordinal);
    }
    return candidate;
}

[[nodiscard]] std::string uniqueRegionId(
    std::string_view token,
    std::unordered_set<std::string>& identities)
{
    const auto base = "sample_" + std::string(token);
    auto candidate = base;
    for (std::size_t ordinal = 2;
         identities.contains(candidate);
         ++ordinal)
    {
        candidate = base + "_" + std::to_string(ordinal);
    }
    identities.insert(candidate);
    return candidate;
}

[[nodiscard]] bool sameAudioShape(
    const talas::project::ProjectAsset& left,
    const talas::project::ProjectAsset& right)
{
    return left.kind == talas::project::ProjectAssetKind::Audio
        && right.kind == talas::project::ProjectAssetKind::Audio
        && left.channelCount == right.channelCount
        && left.sampleRate == right.sampleRate
        && left.frameCount == right.frameCount
        && left.encoding == right.encoding;
}

[[nodiscard]] std::filesystem::path comparablePath(
    const std::filesystem::path& path)
{
    std::error_code canonicalError;
    const auto canonical = std::filesystem::weakly_canonical(
        path,
        canonicalError);
    if (!canonicalError)
        return canonical;

    std::error_code absoluteError;
    const auto absolute = std::filesystem::absolute(
        path,
        absoluteError);
    return (absoluteError ? path : absolute).lexically_normal();
}

[[nodiscard]] SampleMapImportResult failure(
    SampleMapImportErrorCode code,
    std::string message,
    const std::filesystem::path& path = {})
{
    SampleMapImportResult result;
    result.error = SampleMapImportError{
        code,
        std::move(message),
        talas::project::pathToUtf8(path),
    };
    return result;
}

[[nodiscard]] SampleMapImportResult rollbackFailure(
    PreparedSampleMapImport& candidate,
    SampleMapImportErrorCode code,
    std::string message,
    const std::filesystem::path& path = {})
{
    const auto cleanup = discardPreparedSampleMapImportFiles(candidate);
    candidate.assets.clear();
    candidate.expectedAssets.clear();
    candidate.regions.clear();
    if (!cleanup.empty())
    {
        SampleMapImportResult result;
        result.candidate = std::move(candidate);
        result.error = SampleMapImportError{
            SampleMapImportErrorCode::CleanupFailed,
            std::move(message) + "; cleanup failed: " + cleanup,
            talas::project::pathToUtf8(path),
        };
        return result;
    }
    return failure(code, std::move(message), path);
}
}

SampleMapImportResult prepareSampleMapImport(
    const talas::project::ProjectDocument& document,
    const SampleMapImportOptions& options)
{
    if (options.sourcePaths.empty())
    {
        return failure(
            SampleMapImportErrorCode::InvalidInput,
            "Sample-map import requires at least one source path");
    }
    if (options.sourcePaths.size()
        > talas::project::maximumProjectSampleRegions)
    {
        return failure(
            SampleMapImportErrorCode::InvalidInput,
            "Sample-map import source count exceeds the supported region limit");
    }

    std::optional<std::filesystem::path> effectiveProjectPath =
        options.projectFilePath;
    std::optional<std::filesystem::path> packageDirectory;
    if (options.projectFilePath.has_value())
    {
        const auto resolved = talas::project::resolveProjectPackagePath(
            *options.projectFilePath);
        if (resolved.ok())
        {
            effectiveProjectPath = resolved.projectFilePath;
            packageDirectory = resolved.packageDirectory;
        }
        else if (options.disposition
                 == SampleMapImportDisposition::Embed)
        {
            return failure(
                SampleMapImportErrorCode::InvalidInput,
                resolved.error,
                *options.projectFilePath);
        }
    }
    if (options.disposition == SampleMapImportDisposition::Embed
        && !packageDirectory.has_value())
    {
        return failure(
            SampleMapImportErrorCode::InvalidInput,
            "Embedded sample-map import requires an open project package",
            options.projectFilePath.value_or(std::filesystem::path{}));
    }
    if (cancellationRequested(options))
    {
        return failure(
            SampleMapImportErrorCode::Cancelled,
            "Sample-map import cancelled");
    }

    struct SortablePath
    {
        std::filesystem::path sourcePath;
        std::string foldedBasename;
        std::string normalizedPath;
    };
    std::vector<SortablePath> sortedPaths;
    sortedPaths.reserve(options.sourcePaths.size());
    for (const auto& sourcePath : options.sourcePaths)
    {
        std::error_code absoluteError;
        const auto absolute = std::filesystem::absolute(
            sourcePath,
            absoluteError);
        const auto normalized =
            (absoluteError ? sourcePath : absolute).lexically_normal();
        sortedPaths.push_back({
            sourcePath,
            asciiCaseFold(talas::project::pathToUtf8(sourcePath.filename())),
            talas::project::pathToUtf8(normalized),
        });
    }
    std::ranges::sort(
        sortedPaths,
        [](const auto& left, const auto& right) {
            if (left.foldedBasename != right.foldedBasename)
            {
                return left.foldedBasename
                    < right.foldedBasename;
            }
            return left.normalizedPath < right.normalizedPath;
        });

    const auto inspectionProgressShare =
        options.disposition == SampleMapImportDisposition::Embed
        ? 0.5
        : 1.0;
    std::vector<InspectedSource> inspectedSources;
    inspectedSources.reserve(sortedPaths.size());
    for (std::size_t index = 0; index < sortedPaths.size(); ++index)
    {
        if (cancellationRequested(options))
        {
            return failure(
                SampleMapImportErrorCode::Cancelled,
                "Sample-map import cancelled");
        }
        std::error_code canonicalError;
        const auto canonical = std::filesystem::canonical(
            sortedPaths[index].sourcePath,
            canonicalError);
        if (canonicalError)
        {
            return failure(
                SampleMapImportErrorCode::InspectionFailed,
                "Unable to resolve sample-map import source: "
                    + canonicalError.message(),
                sortedPaths[index].sourcePath);
        }

        talas::project::ProjectAsset manifest;
        manifest.id = "inspection_" + std::to_string(index + 1U);
        manifest.kind = talas::project::ProjectAssetKind::Audio;
        manifest.path = talas::project::pathToUtf8(canonical);
        const auto inspected = talas::project::inspectProjectAsset(
            manifest,
            canonical,
            options.shouldCancel);
        if (!inspected.ok())
        {
            const auto wasCancelled =
                inspected.error->code
                == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
            return failure(
                wasCancelled
                    ? SampleMapImportErrorCode::Cancelled
                    : SampleMapImportErrorCode::InspectionFailed,
                inspected.error->message,
                canonical);
        }
        inspectedSources.push_back({
            canonical,
            talas::project::pathToUtf8(canonical.filename()),
            talas::project::pathToUtf8(canonical.lexically_normal()),
            inspected.asset,
        });
        reportProgress(
            options,
            inspectionProgressShare
                * static_cast<double>(index + 1U)
                / static_cast<double>(sortedPaths.size()));
    }

    PreparedSampleMapImport candidate;
    auto knownAssets = document.assets;
    std::unordered_set<std::string> regionIds;
    for (const auto& map : document.sampleMapLibrary.maps)
    {
        for (const auto& region : map.regions)
            regionIds.insert(region.regionId);
    }
    std::vector<NewAssetSource> newAssetSources;
    const auto scheduleEmbeddedAdoption = [&](
        const talas::project::ProjectAsset& asset,
        const std::filesystem::path& sourcePath) {
        if (options.disposition != SampleMapImportDisposition::Embed
            || asset.disposition
                == talas::project::ProjectAssetDisposition::Embedded
            || std::ranges::any_of(
                candidate.assets,
                [&](const auto& candidateAsset) {
                    return candidateAsset.id == asset.id;
                }))
        {
            return;
        }
        candidate.assets.push_back(asset);
        candidate.expectedAssets.push_back(asset);
        newAssetSources.push_back({asset.id, sourcePath});
    };
    talas::project::ProjectSampleMapAutoMapRequest autoMap;
    autoMap.lowNote = options.lowNote;
    autoMap.highNote = options.highNote;
    autoMap.anchorRootNote = options.anchorRootNote;
    for (const auto& source : inspectedSources)
    {
        const auto samePath = std::ranges::find_if(
            knownAssets,
            [&](const auto& asset) {
                const auto resolved =
                    talas::project::resolveProjectAssetPath(
                        asset,
                        effectiveProjectPath);
                return !resolved.empty()
                    && comparablePath(resolved) == source.path;
            });
        // Only an Audio asset can share its identity with a sample-map
        // source; a Table holding the same bytes is a different asset.
        const auto exact = std::ranges::find_if(
            knownAssets,
            [&](const auto& asset) {
                return asset.kind == talas::project::ProjectAssetKind::Audio
                    && asset.contentHash == source.asset.contentHash;
            });
        std::string assetId;
        if (samePath != knownAssets.end())
        {
            if (samePath->kind != talas::project::ProjectAssetKind::Audio)
            {
                return failure(
                    SampleMapImportErrorCode::InspectionFailed,
                    "Sample-map source path is already bound to a non-Audio asset",
                    source.path);
            }
            if (!samePath->contentHash.empty())
            {
                if (samePath->contentHash != source.asset.contentHash
                    || !sameAudioShape(*samePath, source.asset))
                {
                    return failure(
                        SampleMapImportErrorCode::InspectionFailed,
                        "Sample-map source path differs from its accepted asset generation",
                        source.path);
                }
                assetId = samePath->id;
                scheduleEmbeddedAdoption(*samePath, source.path);
            }
            else
            {
                auto asset = source.asset;
                asset.id = samePath->id;
                asset.path = samePath->path;
                asset.disposition = samePath->disposition;
                asset.generation = samePath->generation;
                asset.provenance = samePath->provenance;
                if ((samePath->channelCount != 0U
                     && samePath->channelCount != asset.channelCount)
                    || (samePath->sampleRate != 0.0
                        && samePath->sampleRate != asset.sampleRate)
                    || (samePath->frameCount != 0U
                        && samePath->frameCount != asset.frameCount)
                    || (!samePath->encoding.empty()
                        && samePath->encoding != asset.encoding))
                {
                    return failure(
                        SampleMapImportErrorCode::InspectionFailed,
                        "Sample-map source media shape conflicts with its pending manifest",
                        source.path);
                }
                assetId = asset.id;
                candidate.assets.push_back(asset);
                candidate.expectedAssets.push_back(*samePath);
                *samePath = asset;
                newAssetSources.push_back({assetId, source.path});
            }
        }
        else if (exact != knownAssets.end())
        {
            if (!sameAudioShape(*exact, source.asset))
            {
                return failure(
                    SampleMapImportErrorCode::InspectionFailed,
                    "Exact-hash sample-map source has inconsistent media shape",
                    source.path);
            }
            assetId = exact->id;
            scheduleEmbeddedAdoption(*exact, source.path);
        }
        else
        {
            const auto token = identifierToken(source.path);
            assetId = uniqueAssetId(token, knownAssets);
            auto asset = source.asset;
            asset.id = assetId;
            asset.path = talas::project::projectAssetStoredPath(
                source.path,
                effectiveProjectPath);
            asset.disposition =
                talas::project::ProjectAssetDisposition::Referenced;
            candidate.assets.push_back(asset);
            knownAssets.push_back(asset);
            newAssetSources.push_back({assetId, source.path});
        }

        const auto token = identifierToken(source.path);
        autoMap.candidates.push_back({
            uniqueRegionId(token, regionIds),
            std::move(assetId),
            source.basename,
            source.normalizedPath,
        });
    }
    candidate.autoMapCandidates = autoMap.candidates;
    const auto mapped = talas::project::makeAutoMappedSampleRegions(autoMap);
    if (!mapped.ok() && !options.deferAutoMapRangeValidation)
    {
        return failure(
            SampleMapImportErrorCode::InvalidInput,
            mapped.error->path + ": " + mapped.error->message);
    }
    if (mapped.ok())
        candidate.regions = mapped.regions;

    if (options.disposition == SampleMapImportDisposition::Reference)
    {
        reportProgress(options, 1.0);
        return {std::move(candidate), std::nullopt};
    }

    const auto newAssetCount = candidate.assets.size();
    for (std::size_t index = 0; index < newAssetCount; ++index)
    {
        if (cancellationRequested(options))
        {
            return rollbackFailure(
                candidate,
                SampleMapImportErrorCode::Cancelled,
                "Sample-map import cancelled");
        }
        auto& asset = candidate.assets[index];
        const auto source = std::ranges::find(
            newAssetSources,
            asset.id,
            &NewAssetSource::assetId);
        if (source == newAssetSources.end())
        {
            return rollbackFailure(
                candidate,
                SampleMapImportErrorCode::CopyFailed,
                "Sample-map import lost its inspected source identity");
        }
        auto imported = importProjectPackageAsset(
            *packageDirectory,
            source->sourcePath,
            asset.id,
            options.shouldCancel);
        if (!imported.ok())
        {
            const auto importedError = imported.error;
            const auto importedErrorCode = imported.errorCode;
            const auto errorPath = imported.retainedStagingPath.empty()
                ? source->sourcePath
                : imported.retainedStagingPath;
            if (!imported.retainedStagingPath.empty())
                candidate.cleanupImports.push_back(std::move(imported));
            return rollbackFailure(
                candidate,
                importedErrorCode
                        == ProjectPackageImportErrorCode::CleanupFailed
                    ? SampleMapImportErrorCode::CleanupFailed
                    : importedErrorCode
                              == ProjectPackageImportErrorCode::Cancelled
                        ? SampleMapImportErrorCode::Cancelled
                        : SampleMapImportErrorCode::CopyFailed,
                importedError,
                errorPath);
        }
        const auto copiedPath =
            *packageDirectory
            / talas::project::pathFromUtf8(imported.relativePath);
        if (imported.created)
            candidate.createdFiles.push_back(copiedPath);
        asset.path = imported.relativePath;
        asset.disposition =
            talas::project::ProjectAssetDisposition::Embedded;
        const auto verified = talas::project::inspectProjectAsset(
            asset,
            copiedPath,
            options.shouldCancel);
        if (!verified.ok())
        {
            return rollbackFailure(
                candidate,
                verified.error->code
                        == talas::project::ProjectAssetInspectionErrorCode::Cancelled
                    ? SampleMapImportErrorCode::Cancelled
                    : SampleMapImportErrorCode::CopyFailed,
                verified.error->message,
                copiedPath);
        }
        asset = verified.asset;
        reportProgress(
            options,
            0.5
                + 0.5 * static_cast<double>(index + 1U)
                    / static_cast<double>(newAssetCount));
    }
    if (newAssetCount == 0)
        reportProgress(options, 1.0);
    return {std::move(candidate), std::nullopt};
}

std::string discardPreparedSampleMapImportFiles(
    PreparedSampleMapImport& candidate)
{
    std::vector<ProjectPackageImportResult> remainingImports;
    std::string firstError;
    for (auto& imported : candidate.cleanupImports)
    {
        const auto cleanup =
            discardProjectPackageImportArtifacts(imported);
        if (!cleanup.empty())
        {
            if (firstError.empty())
                firstError = cleanup;
            remainingImports.push_back(std::move(imported));
        }
    }
    candidate.cleanupImports = std::move(remainingImports);

    std::vector<std::filesystem::path> remaining;
    for (const auto& path : candidate.createdFiles)
    {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(
            path,
            statusError);
        if (statusError == std::errc::no_such_file_or_directory)
            continue;
        if (!statusError
            && status.type() == std::filesystem::file_type::not_found)
        {
            continue;
        }
        if (statusError)
        {
            if (firstError.empty())
                firstError = statusError.message();
            remaining.push_back(path);
            continue;
        }
        if (!std::filesystem::is_regular_file(status)
            || std::filesystem::is_symlink(status))
        {
            if (firstError.empty())
                firstError = "candidate-created path is not a regular file";
            remaining.push_back(path);
            continue;
        }
        std::error_code removeError;
        if (!std::filesystem::remove(path, removeError)
            || removeError)
        {
            if (firstError.empty())
            {
                firstError = removeError
                    ? removeError.message()
                    : "unable to remove candidate-created file";
            }
            remaining.push_back(path);
        }
    }
    candidate.createdFiles = std::move(remaining);
    return firstError;
}
}
