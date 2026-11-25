#include "talas/project/ProjectAssetResolver.h"

#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <map>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
[[nodiscard]] ProjectAssetCandidateResult failure(
    ProjectAssetInspectionErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    ProjectAssetCandidateResult result;
    result.error = ProjectAssetInspectionError{
        code,
        std::move(message),
        pathToUtf8(path),
    };
    return result;
}

void appendDiagnostic(
    ProjectAssetSearchResult& result,
    const ProjectAssetSearchLimits& limits,
    ProjectAssetResolverDiagnostic diagnostic)
{
    if (result.diagnostics.size() < limits.maximumDiagnostics)
    {
        result.diagnostics.push_back(std::move(diagnostic));
        return;
    }
    if (result.diagnostics.size() == limits.maximumDiagnostics)
    {
        result.diagnostics.push_back({
            ProjectAssetResolverDiagnosticCode::CapacityExceeded,
            {},
            "Additional asset resolver diagnostics were truncated",
        });
    }
}

[[nodiscard]] bool isContainedRelativePath(
    const std::filesystem::path& relative)
{
    if (relative.empty() || relative == ".")
        return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

[[nodiscard]] bool acceptedMediaShapeMatches(
    const ProjectAsset& accepted,
    const ProjectAsset& inspected)
{
    return (accepted.channelCount == 0U
            || accepted.channelCount == inspected.channelCount)
        && (accepted.sampleRate == 0.0
            || accepted.sampleRate == inspected.sampleRate)
        && (accepted.frameCount == 0U
            || accepted.frameCount == inspected.frameCount)
        && (accepted.encoding.empty()
            || accepted.encoding == inspected.encoding);
}
}

ProjectAssetCandidateResult classifyProjectAssetCandidate(
    const ProjectAsset& manifest,
    const std::filesystem::path& candidatePath,
    const std::optional<std::filesystem::path>& projectFilePath,
    ProjectAssetInspectionCancellationCheck shouldCancel)
{
    if (manifest.disposition != ProjectAssetDisposition::Referenced)
    {
        return failure(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Embedded assets cannot be redirected to an external candidate",
            candidatePath);
    }
    if (shouldCancel && shouldCancel())
    {
        return failure(
            ProjectAssetInspectionErrorCode::Cancelled,
            "Asset inspection cancelled",
            candidatePath);
    }

    std::error_code canonicalError;
    const auto canonicalCandidate =
        std::filesystem::canonical(candidatePath, canonicalError);
    if (canonicalError)
    {
        return failure(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to resolve asset candidate: "
                + canonicalError.message(),
            candidatePath);
    }

    auto inspectable = manifest;
    inspectable.contentHash.clear();
    if (!manifest.contentHash.empty()
        && (manifest.kind == ProjectAssetKind::Audio
            || manifest.kind
                == ProjectAssetKind::PartialSpectrum))
    {
        inspectable.channelCount = 0U;
        inspectable.sampleRate = 0.0;
        inspectable.frameCount = 0U;
        inspectable.encoding.clear();
    }
    const auto inspected = inspectProjectAsset(
        inspectable,
        canonicalCandidate,
        shouldCancel);
    if (!inspected.ok())
    {
        ProjectAssetCandidateResult result;
        result.error = inspected.error;
        return result;
    }

    const auto recanonicalCandidate =
        std::filesystem::canonical(candidatePath, canonicalError);
    if (canonicalError || recanonicalCandidate != canonicalCandidate)
    {
        return failure(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset candidate target changed during local inspection",
            candidatePath);
    }

    const auto stableHash = calculateProjectAssetSha256(
        canonicalCandidate,
        shouldCancel);
    if (!stableHash.ok())
    {
        ProjectAssetCandidateResult result;
        result.error = stableHash.error;
        return result;
    }
    if (stableHash.contentHash != inspected.asset.contentHash)
    {
        return failure(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset candidate changed during local inspection",
            candidatePath);
    }
    const auto finalCanonicalCandidate =
        std::filesystem::canonical(candidatePath, canonicalError);
    if (canonicalError || finalCanonicalCandidate != canonicalCandidate)
    {
        return failure(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset candidate target changed during local inspection",
            candidatePath);
    }

    auto candidate = inspected.asset;
    candidate.path = projectAssetStoredPath(
        canonicalCandidate,
        projectFilePath);

    ProjectAssetResolutionMode mode;
    if (manifest.contentHash.empty())
    {
        mode = ProjectAssetResolutionMode::InitialInspection;
    }
    else if (candidate.contentHash == manifest.contentHash)
    {
        if ((manifest.kind == ProjectAssetKind::Audio
                || manifest.kind
                    == ProjectAssetKind::PartialSpectrum)
            && !acceptedMediaShapeMatches(manifest, candidate))
        {
            return failure(
                ProjectAssetInspectionErrorCode::MediaMismatch,
                "Asset media shape does not match the accepted manifest",
                candidatePath);
        }
        mode = ProjectAssetResolutionMode::Relocation;
    }
    else
    {
        mode = ProjectAssetResolutionMode::Replacement;
    }

    ProjectAssetCandidateResult result;
    result.resolution = ProjectAssetResolution{
        mode,
        manifest.generation,
        manifest.contentHash,
        std::move(candidate),
    };
    return result;
}

ProjectAssetSearchResult searchProjectAssetDirectory(
    std::span<const ProjectAsset> manifests,
    const std::filesystem::path& searchRoot,
    const std::optional<std::filesystem::path>& projectFilePath,
    ProjectAssetSearchLimits limits,
    ProjectAssetInspectionCancellationCheck shouldCancel,
    ProjectAssetSearchProgressCallback onProgress)
{
    ProjectAssetSearchResult result;
    const auto cancelled = [&] {
        return shouldCancel && shouldCancel();
    };
    if (cancelled())
    {
        result.status = ProjectAssetSearchStatus::Cancelled;
        return result;
    }
    if (limits.maximumVisitedEntries == 0
        || limits.maximumHashedFiles == 0)
    {
        result.status = ProjectAssetSearchStatus::CapacityExceeded;
        appendDiagnostic(
            result,
            limits,
            {
                ProjectAssetResolverDiagnosticCode::CapacityExceeded,
                pathToUtf8(searchRoot),
                "Asset search capacity must allow at least one entry and file",
            });
        return result;
    }

    std::error_code rootError;
    const auto canonicalRoot =
        std::filesystem::canonical(searchRoot, rootError);
    if (rootError)
    {
        result.status = ProjectAssetSearchStatus::InvalidRoot;
        appendDiagnostic(
            result,
            limits,
            {
                ProjectAssetResolverDiagnosticCode::Inaccessible,
                pathToUtf8(searchRoot),
                "Unable to resolve asset search root: "
                    + rootError.message(),
            });
        return result;
    }
    const auto rootStatus =
        std::filesystem::status(canonicalRoot, rootError);
    if (rootError || !std::filesystem::is_directory(rootStatus))
    {
        result.status = ProjectAssetSearchStatus::InvalidRoot;
        appendDiagnostic(
            result,
            limits,
            {
                ProjectAssetResolverDiagnosticCode::NotRegularFile,
                pathToUtf8(canonicalRoot),
                "Asset search root must be a readable directory",
            });
        return result;
    }

    std::map<std::string, std::vector<std::size_t>> targetHashes;
    std::vector<const ProjectAsset*> matchManifests;
    for (const auto& manifest : manifests)
    {
        if (manifest.disposition != ProjectAssetDisposition::Referenced
            || !isCanonicalProjectAssetContentHash(
                manifest.contentHash))
        {
            continue;
        }
        const auto matchIndex = result.matches.size();
        result.matches.push_back({manifest.id, {}});
        matchManifests.push_back(&manifest);
        targetHashes[manifest.contentHash].push_back(matchIndex);
    }
    if (targetHashes.empty())
        return result;

    std::vector<std::filesystem::path> pendingDirectories{
        canonicalRoot,
    };
    while (!pendingDirectories.empty())
    {
        if (cancelled())
        {
            result.status = ProjectAssetSearchStatus::Cancelled;
            result.matches.clear();
            return result;
        }
        auto directory = std::move(pendingDirectories.back());
        pendingDirectories.pop_back();

        std::error_code iterationError;
        std::filesystem::directory_iterator iterator(
            directory,
            std::filesystem::directory_options::skip_permission_denied,
            iterationError);
        if (iterationError)
        {
            appendDiagnostic(
                result,
                limits,
                {
                    ProjectAssetResolverDiagnosticCode::Inaccessible,
                    pathToUtf8(directory),
                    "Unable to inspect directory: "
                        + iterationError.message(),
                });
            continue;
        }

        std::vector<std::filesystem::directory_entry> entries;
        for (const auto end = std::filesystem::directory_iterator{};
             iterator != end;
             iterator.increment(iterationError))
        {
            if (cancelled())
            {
                result.status = ProjectAssetSearchStatus::Cancelled;
                result.matches.clear();
                return result;
            }
            if (iterationError)
            {
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::Inaccessible,
                        pathToUtf8(directory),
                        "Unable to continue directory inspection: "
                            + iterationError.message(),
                    });
                iterationError.clear();
                continue;
            }
            if (result.visitedEntries
                >= limits.maximumVisitedEntries)
            {
                result.status =
                    ProjectAssetSearchStatus::CapacityExceeded;
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::CapacityExceeded,
                        pathToUtf8(iterator->path()),
                        "Asset search visited-entry capacity exceeded; select a narrower folder",
                    });
                result.matches.clear();
                return result;
            }
            ++result.visitedEntries;
            entries.push_back(*iterator);
        }
        std::ranges::sort(entries, {}, [](const auto& entry) {
            return pathToUtf8(entry.path());
        });

        std::vector<std::filesystem::path> childDirectories;
        for (const auto& entry : entries)
        {
            if (cancelled())
            {
                result.status = ProjectAssetSearchStatus::Cancelled;
                result.matches.clear();
                return result;
            }
            std::error_code statusError;
            const auto status = entry.symlink_status(statusError);
            if (statusError)
            {
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::Inaccessible,
                        pathToUtf8(entry.path()),
                        "Unable to inspect search entry: "
                            + statusError.message(),
                    });
                continue;
            }
            if (std::filesystem::is_symlink(status))
                continue;
            if (std::filesystem::is_directory(status))
            {
                childDirectories.push_back(entry.path());
                continue;
            }
            if (!std::filesystem::is_regular_file(status))
                continue;
            if (result.hashedFiles >= limits.maximumHashedFiles)
            {
                result.status =
                    ProjectAssetSearchStatus::CapacityExceeded;
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::CapacityExceeded,
                        pathToUtf8(entry.path()),
                        "Asset search hash capacity exceeded; select a narrower folder",
                    });
                result.matches.clear();
                return result;
            }

            std::error_code candidateError;
            const auto canonicalCandidate =
                std::filesystem::canonical(
                    entry.path(),
                    candidateError);
            if (candidateError)
            {
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::Inaccessible,
                        pathToUtf8(entry.path()),
                        "Unable to resolve asset candidate: "
                            + candidateError.message(),
                    });
                continue;
            }
            const auto relative =
                canonicalCandidate.lexically_relative(canonicalRoot);
            if (!isContainedRelativePath(relative))
            {
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::Inaccessible,
                        pathToUtf8(canonicalCandidate),
                        "Asset candidate resolves outside the selected search root",
                    });
                continue;
            }

            ++result.hashedFiles;
            const auto hash = calculateProjectAssetSha256(
                canonicalCandidate,
                shouldCancel);
            if (!hash.ok())
            {
                if (hash.error->code
                    == ProjectAssetInspectionErrorCode::Cancelled)
                {
                    result.status = ProjectAssetSearchStatus::Cancelled;
                    result.matches.clear();
                    return result;
                }
                appendDiagnostic(
                    result,
                    limits,
                    {
                        ProjectAssetResolverDiagnosticCode::Inaccessible,
                        hash.error->path,
                        hash.error->message,
                    });
                continue;
            }

            if (const auto targets = targetHashes.find(hash.contentHash);
                targets != targetHashes.end())
            {
                std::error_code stableStatusError;
                const auto stableStatus =
                    std::filesystem::symlink_status(
                        entry.path(),
                        stableStatusError);
                std::error_code stableCanonicalError;
                const auto stableCanonicalCandidate =
                    std::filesystem::canonical(
                        entry.path(),
                        stableCanonicalError);
                const auto stableRelative = stableCanonicalError
                    ? std::filesystem::path{}
                    : stableCanonicalCandidate.lexically_relative(
                          canonicalRoot);
                if (stableStatusError
                    || std::filesystem::is_symlink(stableStatus)
                    || !std::filesystem::is_regular_file(stableStatus)
                    || stableCanonicalError
                    || stableCanonicalCandidate != canonicalCandidate
                    || !isContainedRelativePath(stableRelative))
                {
                    appendDiagnostic(
                        result,
                        limits,
                        {
                            ProjectAssetResolverDiagnosticCode::Inaccessible,
                            pathToUtf8(entry.path()),
                            "Asset candidate target changed during directory search",
                        });
                    continue;
                }

                const auto stableHash = calculateProjectAssetSha256(
                    canonicalCandidate,
                    shouldCancel);
                if (!stableHash.ok())
                {
                    if (stableHash.error->code
                        == ProjectAssetInspectionErrorCode::Cancelled)
                    {
                        result.status = ProjectAssetSearchStatus::Cancelled;
                        result.matches.clear();
                        return result;
                    }
                    appendDiagnostic(
                        result,
                        limits,
                        {
                            ProjectAssetResolverDiagnosticCode::Inaccessible,
                            stableHash.error->path,
                            stableHash.error->message,
                        });
                    continue;
                }

                std::optional<ProjectAsset> inspectedAudioCandidate;
                std::optional<ProjectAssetInspectionError>
                    audioInspectionError;
                const auto audioTarget = std::ranges::find_if(
                    targets->second,
                    [&](const auto matchIndex) {
                        return matchManifests[matchIndex]->kind
                            == ProjectAssetKind::Audio;
                    });
                if (audioTarget != targets->second.end())
                {
                    auto inspectable = *matchManifests[*audioTarget];
                    inspectable.contentHash = hash.contentHash;
                    inspectable.channelCount = 0U;
                    inspectable.sampleRate = 0.0;
                    inspectable.frameCount = 0U;
                    inspectable.encoding.clear();
                    auto inspectedAudio = inspectProjectAsset(
                        inspectable,
                        canonicalCandidate,
                        shouldCancel);
                    if (!inspectedAudio.ok()
                        && inspectedAudio.error->code
                            == ProjectAssetInspectionErrorCode::Cancelled)
                    {
                        result.status = ProjectAssetSearchStatus::Cancelled;
                        result.matches.clear();
                        return result;
                    }
                    if (inspectedAudio.ok())
                    {
                        inspectedAudioCandidate =
                            std::move(inspectedAudio.asset);
                    }
                    else
                    {
                        audioInspectionError =
                            std::move(inspectedAudio.error);
                    }

                    const auto postAudioHash =
                        calculateProjectAssetSha256(
                            canonicalCandidate,
                            shouldCancel);
                    if (!postAudioHash.ok())
                    {
                        if (postAudioHash.error->code
                            == ProjectAssetInspectionErrorCode::Cancelled)
                        {
                            result.status =
                                ProjectAssetSearchStatus::Cancelled;
                            result.matches.clear();
                            return result;
                        }
                        appendDiagnostic(
                            result,
                            limits,
                            {
                                ProjectAssetResolverDiagnosticCode::Inaccessible,
                                postAudioHash.error->path,
                                postAudioHash.error->message,
                            });
                        continue;
                    }
                    if (postAudioHash.contentHash != hash.contentHash)
                    {
                        appendDiagnostic(
                            result,
                            limits,
                            {
                                ProjectAssetResolverDiagnosticCode::Inaccessible,
                                pathToUtf8(entry.path()),
                                "Audio asset candidate changed during media inspection",
                            });
                        continue;
                    }
                }

                std::error_code finalStatusError;
                const auto finalStatus =
                    std::filesystem::symlink_status(
                        entry.path(),
                        finalStatusError);
                std::error_code finalCanonicalError;
                const auto finalCanonicalCandidate =
                    std::filesystem::canonical(
                        entry.path(),
                        finalCanonicalError);
                if (stableHash.contentHash != hash.contentHash
                    || finalStatusError
                    || std::filesystem::is_symlink(finalStatus)
                    || !std::filesystem::is_regular_file(finalStatus)
                    || finalCanonicalError
                    || finalCanonicalCandidate != canonicalCandidate)
                {
                    appendDiagnostic(
                        result,
                        limits,
                        {
                            ProjectAssetResolverDiagnosticCode::Inaccessible,
                            pathToUtf8(entry.path()),
                            "Asset candidate changed during directory search",
                        });
                    continue;
                }

                for (const auto matchIndex : targets->second)
                {
                    const auto* manifest = matchManifests[matchIndex];
                    if (manifest->kind == ProjectAssetKind::Audio
                        && (!inspectedAudioCandidate.has_value()
                            || !acceptedMediaShapeMatches(
                                *manifest,
                                *inspectedAudioCandidate)))
                    {
                        appendDiagnostic(
                            result,
                            limits,
                            {
                                ProjectAssetResolverDiagnosticCode::Inaccessible,
                                pathToUtf8(entry.path()),
                                audioInspectionError.has_value()
                                    ? audioInspectionError->message
                                    : "Audio asset media shape does not match the accepted manifest for '"
                                        + manifest->id + "'",
                            });
                        continue;
                    }
                    auto candidate = *manifest;
                    if (manifest->kind
                        == ProjectAssetKind::PartialSpectrum)
                    {
                        auto inspectedPartial = inspectProjectAsset(
                            *manifest,
                            canonicalCandidate,
                            shouldCancel);
                        if (!inspectedPartial.ok())
                        {
                            if (inspectedPartial.error->code
                                == ProjectAssetInspectionErrorCode::Cancelled)
                            {
                                result.status =
                                    ProjectAssetSearchStatus::Cancelled;
                                result.matches.clear();
                                return result;
                            }
                            appendDiagnostic(
                                result,
                                limits,
                                {
                                    ProjectAssetResolverDiagnosticCode::Inaccessible,
                                    pathToUtf8(entry.path()),
                                    inspectedPartial.error->message,
                                });
                            continue;
                        }
                        candidate = std::move(inspectedPartial.asset);
                    }
                    candidate.path = projectAssetStoredPath(
                        canonicalCandidate,
                        projectFilePath);
                    result.matches[matchIndex].candidates.push_back({
                        ProjectAssetResolutionMode::Relocation,
                        manifest->generation,
                        manifest->contentHash,
                        std::move(candidate),
                    });
                }
            }

            if (onProgress)
            {
                onProgress({
                    result.visitedEntries,
                    result.hashedFiles,
                    pathToUtf8(relative),
                });
            }
        }

        for (auto child = childDirectories.rbegin();
             child != childDirectories.rend();
             ++child)
        {
            pendingDirectories.push_back(*child);
        }
    }
    return result;
}
}
