#include "talas/audio/ProjectPackager.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace talas::audio
{
namespace
{
constexpr std::size_t copyBufferBytes = 1024 * 1024;
std::atomic<std::uint64_t> transientArtifactSequence = 0;

struct SourceCopy
{
    std::filesystem::path sourcePath;
    std::filesystem::path relativeDestination;
    std::uintmax_t byteCount = 0;
};

[[nodiscard]] std::filesystem::path siblingWithSuffix(
    const std::filesystem::path& path,
    std::string_view suffix)
{
    auto sibling = path;
    sibling += suffix;
    return sibling;
}

[[nodiscard]] std::filesystem::path uniqueSibling(
    const std::filesystem::path& path,
    std::string_view role)
{
    auto sibling = path;
    const auto stamp =
        std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
    sibling += "." + std::string(role)
        + "-" + std::to_string(stamp)
        + "-" + std::to_string(
            transientArtifactSequence.fetch_add(
                1,
                std::memory_order_relaxed));
    return sibling;
}

struct StagingDirectoryResult
{
    std::filesystem::path path;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return error.empty();
    }
};

[[nodiscard]] StagingDirectoryResult
createUniqueStagingDirectory(
    const std::filesystem::path& destination,
    std::string_view role)
{
    for (std::size_t attempt = 0; attempt < 64; ++attempt)
    {
        auto candidate =
            uniqueSibling(destination, role);
        std::error_code error;
        if (std::filesystem::create_directory(
                candidate,
                error))
        {
            return {std::move(candidate), {}};
        }
        if (!error
            || error == std::errc::file_exists)
        {
            continue;
        }
        return {
            {},
            "Unable to create unique staging directory: "
                + error.message(),
        };
    }
    return {
        {},
        "Unable to reserve a unique staging directory",
    };
}

enum class NoReplacePublishResult
{
    Published,
    DestinationExists,
    Failed,
};

[[nodiscard]] NoReplacePublishResult
publishFileWithoutReplacing(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    std::string& errorMessage)
{
#if defined(_WIN32)
    if (::MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_WRITE_THROUGH)
        != 0)
    {
        return NoReplacePublishResult::Published;
    }
    const auto nativeError = ::GetLastError();
    if (nativeError == ERROR_FILE_EXISTS
        || nativeError == ERROR_ALREADY_EXISTS)
    {
        return NoReplacePublishResult::
            DestinationExists;
    }
    errorMessage = std::error_code{
        static_cast<int>(nativeError),
        std::system_category()}
                       .message();
    return NoReplacePublishResult::Failed;
#elif defined(__APPLE__)
    if (::renamex_np(
            temporary.c_str(),
            destination.c_str(),
            RENAME_EXCL)
        == 0)
    {
        return NoReplacePublishResult::Published;
    }
    if (errno == EEXIST || errno == ENOTEMPTY)
    {
        return NoReplacePublishResult::
            DestinationExists;
    }
    errorMessage =
        std::error_code{
            errno,
            std::generic_category()}
            .message();
    return NoReplacePublishResult::Failed;
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int renameNoReplace = 1;
    if (::syscall(
            SYS_renameat2,
            AT_FDCWD,
            temporary.c_str(),
            AT_FDCWD,
            destination.c_str(),
            renameNoReplace)
        == 0)
    {
        return NoReplacePublishResult::Published;
    }
    if (errno == EEXIST || errno == ENOTEMPTY)
    {
        return NoReplacePublishResult::
            DestinationExists;
    }
    errorMessage =
        std::error_code{
            errno,
            std::generic_category()}
            .message();
    return NoReplacePublishResult::Failed;
#else
    if (::link(
            temporary.c_str(),
            destination.c_str())
        == 0)
    {
        std::error_code ignored;
        std::filesystem::remove(
            temporary,
            ignored);
        return NoReplacePublishResult::Published;
    }
    if (errno == EEXIST || errno == ENOTEMPTY)
    {
        return NoReplacePublishResult::
            DestinationExists;
    }
    errorMessage =
        std::error_code{
            errno,
            std::generic_category()}
            .message();
    return NoReplacePublishResult::Failed;
#endif
}

[[nodiscard]] std::filesystem::file_status safeSymlinkStatus(
    const std::filesystem::path& path,
    std::error_code& error)
{
    auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory)
    {
        error.clear();
        status = std::filesystem::file_status{
            std::filesystem::file_type::not_found};
    }
    return status;
}

[[nodiscard]] bool missing(
    const std::filesystem::file_status& status) noexcept
{
    return status.type() == std::filesystem::file_type::not_found;
}

[[nodiscard]] std::optional<std::string> removeStagingArtifact(
    const std::filesystem::path& path,
    std::string_view description)
{
    std::error_code error;
    const auto status = safeSymlinkStatus(path, error);
    if (error)
    {
        return "Unable to inspect " + std::string(description)
            + ": " + error.message();
    }
    if (missing(status))
        return {};

    if (status.type() == std::filesystem::file_type::symlink
        || std::filesystem::is_regular_file(status))
    {
        if (!std::filesystem::remove(path, error) || error)
        {
            return "Unable to remove " + std::string(description)
                + (error ? ": " + error.message() : "");
        }
        return {};
    }
    if (!std::filesystem::is_directory(status))
    {
        return std::string(description)
            + " is not a removable file or directory";
    }

    std::filesystem::remove_all(path, error);
    if (error)
    {
        return "Unable to remove " + std::string(description)
            + ": " + error.message();
    }
    return {};
}

[[nodiscard]] ProjectPackageResult failure(
    ProjectPackageErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    ProjectPackageResult result;
    result.message = message;
    result.error = ProjectPackageError{
        code,
        std::move(message),
        talas::project::pathToUtf8(path),
    };
    return result;
}

[[nodiscard]] bool cancellationRequested(
    const ProjectPackageOptions& options)
{
    return options.shouldCancel && options.shouldCancel();
}

void reportProgress(
    const ProjectPackageOptions& options,
    double progress)
{
    if (options.reportProgress)
    {
        options.reportProgress(
            std::clamp(progress, 0.0, 1.0));
    }
}

[[nodiscard]] bool isPathWithin(
    const std::filesystem::path& child,
    const std::filesystem::path& parent)
{
    const auto normalizedChild = child.lexically_normal();
    const auto normalizedParent = parent.lexically_normal();
    auto childIterator = normalizedChild.begin();
    for (auto parentIterator = normalizedParent.begin();
         parentIterator != normalizedParent.end();
         ++parentIterator, ++childIterator)
    {
        if (childIterator == normalizedChild.end()
            || *childIterator != *parentIterator)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string sanitizedFileName(
    const std::filesystem::path& sourcePath)
{
    auto sourceName =
        talas::project::pathToUtf8(sourcePath.filename());
    std::string sanitized;
    sanitized.reserve(
        std::min<std::size_t>(sourceName.size(), 96));
    auto previousWasReplacement = false;
    for (const auto byte : sourceName)
    {
        const auto character =
            static_cast<unsigned char>(byte);
        const auto allowed =
            std::isalnum(character) != 0
            || character == '.'
            || character == '-'
            || character == '_';
        if (allowed)
        {
            sanitized.push_back(static_cast<char>(character));
            previousWasReplacement = false;
        }
        else if (!previousWasReplacement)
        {
            sanitized.push_back('_');
            previousWasReplacement = true;
        }
        if (sanitized.size() == 96)
            break;
    }
    while (!sanitized.empty()
           && (sanitized.front() == '.'
               || sanitized.front() == '_'))
    {
        sanitized.erase(sanitized.begin());
    }
    if (sanitized.empty())
        sanitized = "Asset";
    return sanitized;
}

[[nodiscard]] std::string sanitizedPreferredName(
    std::string_view preferredName)
{
    std::string sanitized;
    sanitized.reserve(
        std::min<std::size_t>(preferredName.size(), 80));
    auto previousWasReplacement = false;
    for (const auto byte : preferredName)
    {
        const auto character =
            static_cast<unsigned char>(byte);
        const auto allowed =
            std::isalnum(character) != 0
            || character == '-'
            || character == '_';
        if (allowed)
        {
            sanitized.push_back(static_cast<char>(character));
            previousWasReplacement = false;
        }
        else if (!previousWasReplacement)
        {
            sanitized.push_back('_');
            previousWasReplacement = true;
        }
        if (sanitized.size() == 80)
            break;
    }
    while (!sanitized.empty() && sanitized.front() == '_')
        sanitized.erase(sanitized.begin());
    while (!sanitized.empty() && sanitized.back() == '_')
        sanitized.pop_back();
    if (sanitized.empty())
        sanitized = "Asset";
    return sanitized;
}

[[nodiscard]] std::filesystem::path packagedFileName(
    std::size_t ordinal,
    const std::filesystem::path& sourcePath)
{
    std::ostringstream prefix;
    prefix << std::setw(4) << std::setfill('0')
           << ordinal << '-';
    return prefix.str() + sanitizedFileName(sourcePath);
}

[[nodiscard]] std::optional<std::string> copyFileStreaming(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const std::function<bool()>& shouldCancel,
    const std::function<void(std::size_t)>& didCopy)
{
    std::ifstream input(source, std::ios::binary);
    if (!input)
    {
        return "Unable to open source asset for reading";
    }
    std::ofstream output(
        destination,
        std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return "Unable to create packaged asset";
    }

    // Heap-allocated: this runs on Workbench worker threads whose stacks
    // (512 KiB on macOS) cannot hold a 1 MiB copy buffer.
    std::vector<char> buffer(copyBufferBytes);
    while (input)
    {
        if (shouldCancel && shouldCancel())
        {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(destination, ignored);
            return "cancelled";
        }
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const auto copied =
            static_cast<std::size_t>(input.gcount());
        if (copied == 0)
            break;
        output.write(
            buffer.data(),
            static_cast<std::streamsize>(copied));
        if (!output.good())
        {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(destination, ignored);
            return "Unable to write packaged asset";
        }
        if (didCopy)
            didCopy(copied);
    }
    if (input.bad())
    {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        return "Unable to finish reading source asset";
    }
    output.flush();
    if (!output.good())
    {
        output.close();
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        return "Unable to flush packaged asset";
    }
    return {};
}

enum class FileComparison
{
    Different,
    Equal,
    Cancelled,
};

[[nodiscard]] FileComparison compareFileBytes(
    const std::filesystem::path& first,
    const std::filesystem::path& second,
    const std::function<bool()>& shouldCancel)
{
    std::error_code error;
    const auto firstSize =
        std::filesystem::file_size(first, error);
    if (error)
        return FileComparison::Different;
    const auto secondSize =
        std::filesystem::file_size(second, error);
    if (error || firstSize != secondSize)
    {
        return FileComparison::Different;
    }
    std::ifstream left(first, std::ios::binary);
    std::ifstream right(second, std::ios::binary);
    if (!left || !right)
        return FileComparison::Different;
    std::array<char, 64 * 1024> leftBuffer{};
    std::array<char, 64 * 1024> rightBuffer{};
    while (left && right)
    {
        if (shouldCancel && shouldCancel())
            return FileComparison::Cancelled;
        left.read(
            leftBuffer.data(),
            static_cast<std::streamsize>(
                leftBuffer.size()));
        right.read(
            rightBuffer.data(),
            static_cast<std::streamsize>(
                rightBuffer.size()));
        const auto leftCount = left.gcount();
        const auto rightCount = right.gcount();
        if (leftCount != rightCount
            || !std::equal(
                leftBuffer.begin(),
                leftBuffer.begin() + leftCount,
                rightBuffer.begin()))
        {
            return FileComparison::Different;
        }
    }
    return !left.bad() && !right.bad()
        ? FileComparison::Equal
        : FileComparison::Different;
}

[[nodiscard]] std::filesystem::path collisionPath(
    const std::filesystem::path& basePath,
    std::size_t ordinal)
{
    if (ordinal == 1)
        return basePath;
    const auto extension = basePath.extension();
    auto stem = basePath.stem();
    stem += "-" + std::to_string(ordinal);
    return basePath.parent_path() / (stem += extension);
}
}

ProjectPackageResult saveProjectPackage(
    const talas::project::ProjectDocument& document,
    const ProjectPackageOptions& options)
{
    const auto& outputPath = options.outputPackagePath;
    if (outputPath.empty())
    {
        return failure(
            ProjectPackageErrorCode::InvalidInput,
            "Project package path cannot be empty",
            outputPath);
    }
    auto outputExtension =
        talas::project::pathToUtf8(outputPath.extension());
    std::ranges::transform(
        outputExtension,
        outputExtension.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (outputExtension != talas::project::projectPackageExtension)
    {
        return failure(
            ProjectPackageErrorCode::InvalidInput,
            "Project package path must use the .talaspkg suffix",
            outputPath);
    }
    if (cancellationRequested(options))
    {
        return failure(
            ProjectPackageErrorCode::Cancelled,
            "Project packaging cancelled",
            outputPath);
    }
    const auto inputValidation =
        talas::project::validateProjectDocumentForPersistence(
            document);
    if (!inputValidation.ok())
    {
        const auto& diagnostic =
            inputValidation.diagnostics.front();
        return failure(
            ProjectPackageErrorCode::InvalidInput,
            "Project is not canonically packageable at "
                + diagnostic.path + ": "
                + diagnostic.message,
            outputPath);
    }

    std::error_code error;
    const auto outputStatus =
        safeSymlinkStatus(outputPath, error);
    if (error)
    {
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Unable to inspect package destination: "
                + error.message(),
            outputPath);
    }
    const auto replacingExisting = !missing(outputStatus);
    if (replacingExisting
        && options.sourceProjectFilePath.has_value())
    {
        const auto sourceResolved =
            talas::project::resolveProjectPackagePath(
                *options.sourceProjectFilePath);
        if (sourceResolved.ok()
            && sourceResolved.packageDirectory.has_value())
        {
            const auto equivalent =
                std::filesystem::equivalent(
                    *sourceResolved.packageDirectory,
                    outputPath,
                    error);
            if (error)
            {
                return failure(
                    ProjectPackageErrorCode::WriteFailed,
                    "Unable to compare package source and destination: "
                        + error.message(),
                    outputPath);
            }
            if (equivalent)
            {
                return failure(
                    ProjectPackageErrorCode::InvalidInput,
                    "Cannot replace the package that supplies the current project assets",
                    outputPath);
            }
        }
    }
    if (replacingExisting)
    {
        const auto existing =
            talas::project::resolveProjectPackagePath(outputPath);
        if (!existing.ok()
            || !existing.packageDirectory.has_value())
        {
            return failure(
                ProjectPackageErrorCode::InvalidInput,
                "Refusing to replace an unmarked or invalid package destination",
                outputPath);
        }
    }

    const auto backup =
        siblingWithSuffix(outputPath, ".bak");
    const auto backupStatus =
        safeSymlinkStatus(backup, error);
    if (error)
    {
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Unable to inspect package backup: "
                + error.message(),
            backup);
    }
    if (!missing(backupStatus))
    {
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Package backup exists and requires recovery before replacement",
            backup);
    }
    talas::project::ProjectDocument packagedDocument =
        document;
    std::vector<SourceCopy> sources;
    std::map<std::filesystem::path, std::size_t>
        sourceIndices;
    std::unordered_set<std::string> assetIds;
    std::uintmax_t totalBytes = 0;
    for (std::size_t index = 0;
         index < document.assets.size();
         ++index)
    {
        if (!assetIds.insert(document.assets[index].id).second)
        {
            return failure(
                ProjectPackageErrorCode::InvalidInput,
                "Duplicate project asset id/generation: "
                    + document.assets[index].id + "/"
                    + std::to_string(
                        document.assets[index].generation),
                outputPath);
        }
        const auto resolved =
            talas::project::resolveProjectAssetPath(
                document.assets[index],
                options.sourceProjectFilePath);
        if (resolved.empty() || resolved.is_relative())
        {
            return failure(
                ProjectPackageErrorCode::AssetUnavailable,
                "Asset '" + document.assets[index].id
                    + "' has no resolvable source path",
                resolved);
        }
        const auto canonical =
            std::filesystem::canonical(resolved, error);
        if (error)
        {
            return failure(
                ProjectPackageErrorCode::AssetUnavailable,
                "Unable to resolve asset '"
                    + document.assets[index].id
                    + "': " + error.message(),
                resolved);
        }
        const auto status =
            std::filesystem::status(canonical, error);
        if (error
            || !std::filesystem::is_regular_file(status))
        {
            return failure(
                ProjectPackageErrorCode::AssetUnavailable,
                error
                    ? "Unable to inspect asset '"
                        + document.assets[index].id
                        + "': " + error.message()
                    : "Asset '"
                        + document.assets[index].id
                        + "' is not a regular file",
                canonical);
        }
        const auto bytes =
            std::filesystem::file_size(canonical, error);
        if (error || bytes == 0)
        {
            return failure(
                ProjectPackageErrorCode::AssetUnavailable,
                error
                    ? "Unable to measure asset '"
                        + document.assets[index].id
                        + "': " + error.message()
                    : "Asset '"
                        + document.assets[index].id
                        + "' is empty",
                canonical);
        }

        const auto inspected =
            talas::project::inspectProjectAsset(
                document.assets[index],
                canonical,
                [&] {
                    return cancellationRequested(options);
                });
        if (!inspected.ok())
        {
            return failure(
                inspected.error->code
                        == talas::project::
                            ProjectAssetInspectionErrorCode::Cancelled
                    ? ProjectPackageErrorCode::Cancelled
                    : ProjectPackageErrorCode::AssetUnavailable,
                inspected.error->code
                        == talas::project::
                            ProjectAssetInspectionErrorCode::Cancelled
                    ? "Project packaging cancelled"
                    : "Unable to verify asset '"
                        + document.assets[index].id
                        + "': " + inspected.error->message,
                canonical);
        }
        packagedDocument.assets[index] = inspected.asset;
        packagedDocument.assets[index].disposition =
            talas::project::ProjectAssetDisposition::Embedded;

        auto sourceIndex = sourceIndices.find(canonical);
        if (sourceIndex == sourceIndices.end())
        {
            if (bytes
                > std::numeric_limits<std::uintmax_t>::max()
                    - totalBytes)
            {
                return failure(
                    ProjectPackageErrorCode::AssetUnavailable,
                    "Total package asset size is too large",
                    canonical);
            }
            const auto relative =
                std::filesystem::path{
                    talas::project::
                        projectPackageAssetsDirectoryName}
                / packagedFileName(
                    sources.size() + 1,
                    canonical);
            sourceIndex = sourceIndices
                              .emplace(
                                  canonical,
                                  sources.size())
                              .first;
            sources.push_back({
                canonical,
                relative,
                bytes,
            });
            totalBytes += bytes;
        }
        packagedDocument.assets[index].path =
            talas::project::pathToUtf8(
                sources[sourceIndex->second]
                    .relativeDestination);
    }

    const auto packagedValidation =
        talas::project::validateProjectDocumentForPersistence(
            packagedDocument);
    if (!packagedValidation.ok())
    {
        const auto& diagnostic =
            packagedValidation.diagnostics.front();
        return failure(
            ProjectPackageErrorCode::InvalidInput,
            "Packaged project is not canonically persistable at "
                + diagnostic.path + ": "
                + diagnostic.message,
            outputPath);
    }

    const auto parent = outputPath.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return failure(
                ProjectPackageErrorCode::WriteFailed,
                "Unable to create package parent directory: "
                    + error.message(),
                parent);
        }
    }
    const auto staging =
        createUniqueStagingDirectory(
            outputPath,
            "tmp");
    if (!staging.ok())
    {
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            staging.error,
            outputPath);
    }
    const auto& temporary = staging.path;
    std::filesystem::create_directory(
        temporary
            / talas::project::
                projectPackageAssetsDirectoryName,
        error);
    if (error)
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Unable to create temporary package: "
                + error.message(),
            temporary);
    }

    reportProgress(options, 0.0);
    std::uintmax_t copiedBytes = 0;
    for (const auto& source : sources)
    {
        const auto destination =
            temporary / source.relativeDestination;
        const auto copyError = copyFileStreaming(
            source.sourcePath,
            destination,
            [&] {
                return cancellationRequested(options);
            },
            [&](std::size_t count) {
                copiedBytes += count;
                reportProgress(
                    options,
                    totalBytes == 0
                        ? 0.0
                        : static_cast<double>(
                              copiedBytes)
                            / static_cast<double>(
                                totalBytes));
            });
        if (copyError.has_value())
        {
            const auto cancelled =
                *copyError == "cancelled";
            static_cast<void>(
                removeStagingArtifact(
                    temporary,
                    "temporary package"));
            return failure(
                cancelled
                    ? ProjectPackageErrorCode::Cancelled
                    : ProjectPackageErrorCode::WriteFailed,
                cancelled
                    ? "Project packaging cancelled"
                    : *copyError,
                source.sourcePath);
        }
    }
    if (cancellationRequested(options))
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::Cancelled,
            "Project packaging cancelled",
            outputPath);
    }

    {
        const auto manifestPath =
            temporary
            / talas::project::
                projectPackageManifestFileName;
        std::ofstream manifest(
            manifestPath,
            std::ios::binary | std::ios::trunc);
        manifest
            << talas::project::
                projectPackageManifestContents;
        manifest.flush();
        if (!manifest.good())
        {
            manifest.close();
            static_cast<void>(
                removeStagingArtifact(
                    temporary,
                    "temporary package"));
            return failure(
                ProjectPackageErrorCode::WriteFailed,
                "Unable to write package manifest",
                manifestPath);
        }
    }
    const auto projectPath =
        temporary
        / talas::project::projectPackageDocumentFileName;
    const auto savedProject =
        talas::project::saveProjectFile(
            projectPath,
            packagedDocument);
    if (!savedProject.ok())
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Unable to write packaged project: "
                + savedProject.error->message,
            projectPath);
    }

    const talas::project::ProjectPackagePathResult stagedPackage{
        projectPath,
        temporary,
        temporary,
        {},
    };
    const auto verifiedPackage =
        talas::project::validateProjectPackageAssets(
            packagedDocument,
            stagedPackage,
            [&] {
                return cancellationRequested(options);
            });
    if (!verifiedPackage.ok())
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            verifiedPackage.cancelled
                ? ProjectPackageErrorCode::Cancelled
                : ProjectPackageErrorCode::WriteFailed,
            verifiedPackage.cancelled
                ? "Project packaging cancelled"
                : "Packaged asset verification failed for '"
                    + verifiedPackage.assetId + "': "
                    + verifiedPackage.error,
            projectPath);
    }

    const auto finalStatus =
        safeSymlinkStatus(outputPath, error);
    if (error
        || missing(finalStatus) == replacingExisting)
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            error
                ? "Unable to recheck package destination: "
                    + error.message()
                : "Package destination changed during export",
            outputPath);
    }
    if (replacingExisting)
    {
        const auto existing =
            talas::project::resolveProjectPackagePath(
                outputPath);
        if (!existing.ok()
            || !existing.packageDirectory.has_value())
        {
            static_cast<void>(
                removeStagingArtifact(
                    temporary,
                    "temporary package"));
            return failure(
                ProjectPackageErrorCode::WriteFailed,
                "Package destination became invalid during export",
                outputPath);
        }
    }
    if (options.admitPublication && !options.admitPublication())
    {
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::Cancelled,
            "Project packaging invalidated before publication",
            outputPath);
    }
    if (replacingExisting)
    {
        std::filesystem::rename(
            outputPath,
            backup,
            error);
        if (error)
        {
            static_cast<void>(
                removeStagingArtifact(
                    temporary,
                    "temporary package"));
            return failure(
                ProjectPackageErrorCode::WriteFailed,
                "Unable to preserve existing package: "
                    + error.message(),
                outputPath);
        }
    }

    std::filesystem::rename(
        temporary,
        outputPath,
        error);
    if (error)
    {
        if (replacingExisting)
        {
            std::error_code restoreError;
            std::filesystem::rename(
                backup,
                outputPath,
                restoreError);
            if (restoreError)
            {
                static_cast<void>(
                    removeStagingArtifact(
                        temporary,
                        "temporary package"));
                return failure(
                    ProjectPackageErrorCode::WriteFailed,
                    "Unable to publish project package: "
                        + error.message()
                        + "; unable to restore the previous package: "
                        + restoreError.message()
                        + "; preserved backup: "
                        + talas::project::pathToUtf8(
                            backup),
                    backup);
            }
        }
        static_cast<void>(
            removeStagingArtifact(
                temporary,
                "temporary package"));
        return failure(
            ProjectPackageErrorCode::WriteFailed,
            "Unable to publish project package: "
                + error.message(),
            outputPath);
    }

    ProjectPackageResult result;
    result.assetCount = document.assets.size();
    result.copiedFileCount = sources.size();
    result.copiedByteCount = copiedBytes;
    if (replacingExisting)
    {
        if (const auto cleanup =
                removeStagingArtifact(
                    backup,
                    "previous package backup");
            cleanup.has_value())
        {
            result.warning = *cleanup;
        }
    }
    reportProgress(options, 1.0);
    return result;
}

ProjectPackageImportResult importProjectPackageAsset(
    const std::filesystem::path& packageDirectory,
    const std::filesystem::path& sourcePath,
    std::string_view preferredName,
    std::function<bool()> shouldCancel)
{
    const auto cancellation = [] {
        return ProjectPackageImportResult{
            {},
            "Project package asset import cancelled",
            false,
            ProjectPackageImportErrorCode::Cancelled,
        };
    };
    const auto cleanupFailure = [](
                                    std::string message,
                                    const std::filesystem::path& stagingPath) {
        return ProjectPackageImportResult{
            {},
            std::move(message) + "; retained staging path: "
                + talas::project::pathToUtf8(stagingPath),
            false,
            ProjectPackageImportErrorCode::CleanupFailed,
            stagingPath,
        };
    };
    if (shouldCancel && shouldCancel())
        return cancellation();
    const auto resolvedPackage =
        talas::project::resolveProjectPackagePath(
            packageDirectory);
    if (!resolvedPackage.ok()
        || !resolvedPackage.packageDirectory.has_value())
    {
        return {
            {},
            resolvedPackage.ok()
                ? "Import destination is not a project package"
                : resolvedPackage.error,
            false,
        };
    }

    std::error_code error;
    const auto canonicalSource =
        std::filesystem::canonical(sourcePath, error);
    if (error)
    {
        return {
            {},
            "Unable to resolve imported asset: "
                + error.message(),
            false,
        };
    }
    const auto sourceStatus =
        std::filesystem::status(
            canonicalSource,
            error);
    const auto sourceSize =
        !error
        && std::filesystem::is_regular_file(
            sourceStatus)
        ? std::filesystem::file_size(
              canonicalSource,
              error)
        : 0;
    if (error
        || !std::filesystem::is_regular_file(
            sourceStatus)
        || sourceSize == 0)
    {
        return {
            {},
            error
                ? "Unable to inspect imported asset: "
                    + error.message()
                : "Imported asset must be a non-empty regular file",
            false,
        };
    }

    const auto canonicalAssets =
        std::filesystem::canonical(
            *resolvedPackage.packageDirectory
                / talas::project::
                    projectPackageAssetsDirectoryName,
            error);
    if (error)
    {
        return {
            {},
            "Unable to resolve package Assets directory: "
                + error.message(),
            false,
        };
    }
    if (isPathWithin(canonicalSource, canonicalAssets))
    {
        const auto relative =
            std::filesystem::relative(
                canonicalSource,
                *resolvedPackage.packageDirectory,
                error);
        return {
            error
                ? std::string{}
                : talas::project::pathToUtf8(relative),
            error
                ? "Unable to make existing package asset path relative: "
                    + error.message()
                : std::string{},
            false,
        };
    }

    auto baseName =
        sanitizedPreferredName(preferredName);
    auto extension =
        sanitizedFileName(
            canonicalSource.extension());
    if (canonicalSource.extension().empty())
        extension.clear();
    else if (!extension.starts_with('.'))
        extension.insert(extension.begin(), '.');
    const auto basePath =
        *resolvedPackage.packageDirectory
        / talas::project::
            projectPackageAssetsDirectoryName
        / (baseName + extension);

    for (std::size_t ordinal = 1; ; ++ordinal)
    {
        if (shouldCancel && shouldCancel())
            return cancellation();
        const auto destination =
            collisionPath(basePath, ordinal);
        const auto destinationStatus =
            safeSymlinkStatus(destination, error);
        if (error)
        {
            return {
                {},
                "Unable to inspect package import destination: "
                    + error.message(),
                false,
            };
        }
        if (!missing(destinationStatus))
        {
            const auto comparison =
                std::filesystem::is_regular_file(destinationStatus)
                ? compareFileBytes(
                      canonicalSource,
                      destination,
                      shouldCancel)
                : FileComparison::Different;
            if (comparison == FileComparison::Cancelled)
                return cancellation();
            if (comparison == FileComparison::Equal)
            {
                return {
                    talas::project::pathToUtf8(
                        std::filesystem::relative(
                            destination,
                            *resolvedPackage
                                 .packageDirectory,
                            error)),
                    error
                        ? "Unable to make imported asset path relative: "
                            + error.message()
                        : std::string{},
                    false,
                };
            }
            continue;
        }

        const auto staging =
            createUniqueStagingDirectory(
                destination,
                "tmp");
        if (!staging.ok())
        {
            return {{}, staging.error, false};
        }
        const auto temporary =
            staging.path / "Asset";
        const auto copyError =
            copyFileStreaming(
                canonicalSource,
                temporary,
                shouldCancel,
                {});
        if (copyError.has_value())
        {
            if (const auto cleanup = removeStagingArtifact(
                    staging.path,
                    "temporary import staging directory");
                cleanup.has_value())
            {
                return cleanupFailure(
                    (*copyError == "cancelled"
                         ? "Project package asset import cancelled"
                         : *copyError)
                        + "; cleanup failed: " + *cleanup,
                    staging.path);
            }
            if (*copyError == "cancelled")
                return cancellation();
            return {{}, *copyError, false};
        }

        if (shouldCancel && shouldCancel())
        {
            if (const auto cleanup = removeStagingArtifact(
                    staging.path,
                    "temporary import staging directory");
                cleanup.has_value())
            {
                return cleanupFailure(
                    "Project package asset import cancelled; cleanup failed: "
                        + *cleanup,
                    staging.path);
            }
            return cancellation();
        }

        std::string publishError;
        const auto published =
            publishFileWithoutReplacing(
                temporary,
                destination,
                publishError);
        if (published
            == NoReplacePublishResult::
                DestinationExists)
        {
            static_cast<void>(
                removeStagingArtifact(
                    staging.path,
                    "temporary import staging directory"));
            continue;
        }
        if (published
            == NoReplacePublishResult::Failed)
        {
            static_cast<void>(
                removeStagingArtifact(
                    staging.path,
                    "temporary import staging directory"));
            return {
                {},
                "Unable to publish imported asset: "
                    + publishError,
                false,
            };
        }
        static_cast<void>(
            removeStagingArtifact(
                staging.path,
                "temporary import staging directory"));
        const auto relative =
            destination.lexically_relative(
                *resolvedPackage.packageDirectory);
        return {
            talas::project::pathToUtf8(relative),
            {},
            true,
        };
    }
}

std::string discardProjectPackageImportArtifacts(
    ProjectPackageImportResult& result)
{
    if (result.retainedStagingPath.empty())
        return {};
    const auto cleanup = removeStagingArtifact(
        result.retainedStagingPath,
        "retained package import staging directory");
    if (cleanup.has_value())
        return *cleanup;
    result.retainedStagingPath.clear();
    return {};
}
}
