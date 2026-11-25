#include "talas/project/ProjectFile.h"

#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace talas::project
{
namespace
{
std::atomic<std::uint64_t> transientFileSequence = 0;

[[nodiscard]] ProjectFileError makeError(ProjectFileErrorCode code,
                                         std::string message,
                                         const std::filesystem::path& path)
{
    return {code, std::move(message), pathToUtf8(path)};
}

#if defined(_WIN32)
[[nodiscard]] std::filesystem::path siblingWithSuffix(const std::filesystem::path& path, std::string_view suffix)
{
    auto sibling = path;
    sibling += suffix;
    return sibling;
}
#endif

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
    const std::filesystem::path& path)
{
    for (std::size_t attempt = 0;
         attempt < 64;
         ++attempt)
    {
        auto candidate = path;
        candidate += ".tmp-"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count())
            + "-"
            + std::to_string(
                transientFileSequence.fetch_add(
                    1,
                    std::memory_order_relaxed));
        std::error_code error;
        if (std::filesystem::create_directory(
                candidate,
                error))
        {
            return {
                std::move(candidate),
                {},
            };
        }
        if (!error
            || error == std::errc::file_exists)
        {
            continue;
        }
        return {
            {},
            "Unable to create project staging directory: "
                + error.message(),
        };
    }
    return {
        {},
        "Unable to reserve a unique project staging directory",
    };
}

void removeStagingSilently(
    const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove_all(path, error);
}

#if defined(_WIN32)
void removeSilently(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::remove(path, error);
}
#endif

#if !defined(_WIN32)
// fsync() alone does not flush the drive's write cache on macOS.
[[nodiscard]] bool flushToStorage(const std::filesystem::path& path) noexcept
{
    const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0)
        return false;
    auto flushed = false;
#ifdef F_FULLFSYNC
    flushed = ::fcntl(descriptor, F_FULLFSYNC) == 0;
#endif
    if (!flushed)
        flushed = ::fsync(descriptor) == 0;
    return ::close(descriptor) == 0 && flushed;
}
#endif

// Saving through a symlink replaces the file it names, not the link.
[[nodiscard]] std::filesystem::path resolveSaveDestination(const std::filesystem::path& path)
{
    auto destination = path;
    for (int hop = 0; hop < 40; ++hop)
    {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(destination, error);
        if (error || !std::filesystem::is_symlink(status))
            break;
        const auto target = std::filesystem::read_symlink(destination, error);
        if (error)
            break;
        destination = target.is_absolute() ? target : destination.parent_path() / target;
    }
    return destination;
}
}

ProjectFileRevisionResult captureProjectFileRevision(
    const std::filesystem::path& path)
{
    if (path.empty())
    {
        return {
            {},
            makeError(
                ProjectFileErrorCode::ReadFailed,
                "Project path cannot be empty",
                path),
        };
    }

    std::error_code error;
    const auto status =
        std::filesystem::status(path, error);
    if (error
        == std::errc::no_such_file_or_directory
        || (!error
            && status.type()
                == std::filesystem::
                    file_type::not_found))
    {
        return {
            ProjectFileRevision{false, {}},
            {},
        };
    }
    if (error)
    {
        return {
            {},
            makeError(
                ProjectFileErrorCode::ReadFailed,
                "Unable to inspect project file: "
                    + error.message(),
                path),
        };
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return {
            {},
            makeError(
                ProjectFileErrorCode::ReadFailed,
                "Project path is not a regular file",
                path),
        };
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
    {
        return {
            {},
            makeError(
                ProjectFileErrorCode::ReadFailed,
                "Unable to open project file",
                path),
        };
    }
    std::string contents(
        std::istreambuf_iterator<char>{input},
        {});
    if (!input.good() && !input.eof())
    {
        return {
            {},
            makeError(
                ProjectFileErrorCode::ReadFailed,
                "Unable to read project file",
                path),
        };
    }
    return {
        ProjectFileRevision{
            true,
            std::move(contents),
        },
        {},
    };
}

ProjectFileSaveResult saveProjectFile(const std::filesystem::path& path,
                                      const ProjectDocument& document,
                                      const std::optional<ProjectFileRevision>&
                                          expectedRevision)
{
    if (path.empty())
        return {makeError(ProjectFileErrorCode::WriteFailed, "Project path cannot be empty", path)};

    const auto validation =
        validateProjectDocumentForPersistence(document);
    if (!validation.ok())
    {
        const auto& diagnostic = validation.diagnostics.front();
        return {makeError(
            ProjectFileErrorCode::InvalidProject,
            "Project is not canonically persistable at "
                + diagnostic.path + ": "
                + diagnostic.message,
            path)};
    }

    std::error_code error;
    const auto destination = resolveSaveDestination(path);
    const auto parent = destination.parent_path();
    if (!parent.empty())
    {
        std::filesystem::create_directories(parent, error);
        if (error)
        {
            return {makeError(ProjectFileErrorCode::WriteFailed,
                              "Unable to create project directory: " + error.message(),
                              path)};
        }
    }

    const auto staging =
        createUniqueStagingDirectory(destination);
    if (!staging.ok())
    {
        return {
            makeError(
                ProjectFileErrorCode::WriteFailed,
                staging.error,
                path),
        };
    }
    const auto temporary =
        staging.path / "Project.talas.json";

    const auto serialized =
        serializeProjectDocument(document);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            removeStagingSilently(staging.path);
            return {makeError(ProjectFileErrorCode::WriteFailed, "Unable to create temporary project file", temporary)};
        }

        output << serialized;
        output.close();
        if (!output.good())
        {
            removeStagingSilently(staging.path);
            return {makeError(ProjectFileErrorCode::WriteFailed, "Unable to write temporary project file", temporary)};
        }
    }
#if !defined(_WIN32)
    if (!flushToStorage(temporary))
    {
        removeStagingSilently(staging.path);
        return {makeError(ProjectFileErrorCode::WriteFailed, "Unable to flush temporary project file", temporary)};
    }
#endif

    const auto currentRevision =
        captureProjectFileRevision(path);
    if (!currentRevision.ok())
    {
        removeStagingSilently(staging.path);
        return {
            makeError(
                expectedRevision.has_value()
                    ? ProjectFileErrorCode::
                        ExternalConflict
                    : ProjectFileErrorCode::
                        WriteFailed,
                expectedRevision.has_value()
                    ? "Unable to verify that the project file is unchanged: "
                        + currentRevision.error->message
                    : currentRevision.error->message,
                path),
        };
    }
    if (expectedRevision.has_value()
        && *currentRevision.revision
            != *expectedRevision)
    {
        removeStagingSilently(staging.path);
        return {
            makeError(
                ProjectFileErrorCode::
                    ExternalConflict,
                "Project file changed on disk after it was opened or saved",
                path),
        };
    }
#if defined(_WIN32)
    const auto originalExists =
        currentRevision.revision->exists;
    const auto backup = siblingWithSuffix(destination, ".bak");

    error.clear();
    const auto backupStatus =
        std::filesystem::symlink_status(
            backup,
            error);
    const auto backupIsMissing =
        error
            == std::errc::
                no_such_file_or_directory
        || (!error
            && backupStatus.type()
                == std::filesystem::
                    file_type::not_found);
    if (!backupIsMissing)
    {
        removeStagingSilently(staging.path);
        const auto message = error
            ? "Unable to inspect project backup: " + error.message()
            : "Project backup exists and requires recovery before saving";
        return {makeError(ProjectFileErrorCode::WriteFailed, message, backup)};
    }

    if (originalExists)
    {
        error.clear();
        std::filesystem::rename(destination, backup, error);
        if (error)
        {
            removeStagingSilently(staging.path);
            return {makeError(ProjectFileErrorCode::WriteFailed,
                              "Unable to preserve existing project before replacement: " + error.message(),
                              path)};
        }
    }

    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        std::string restoreFailure;
        if (originalExists)
        {
            std::error_code restoreError;
            std::filesystem::rename(backup, destination, restoreError);
            if (restoreError)
            {
                restoreFailure =
                    "; the preserved original remains at "
                    + pathToUtf8(backup)
                    + " because restoring it failed: "
                    + restoreError.message();
            }
        }
        removeStagingSilently(staging.path);
        return {makeError(ProjectFileErrorCode::WriteFailed,
                          "Unable to replace project file: " + error.message()
                              + restoreFailure,
                          path)};
    }

    if (originalExists)
        removeSilently(backup);
    removeStagingSilently(staging.path);
#else
    // One rename replaces the previous file atomically, so there is never a
    // moment without a project file and no backup that could block saving.
    if (currentRevision.revision->exists)
    {
        std::error_code permissionError;
        const auto permissions =
            std::filesystem::status(destination, permissionError).permissions();
        if (!permissionError)
            std::filesystem::permissions(temporary, permissions, permissionError);
    }
    error.clear();
    std::filesystem::rename(temporary, destination, error);
    if (error)
    {
        removeStagingSilently(staging.path);
        return {makeError(ProjectFileErrorCode::WriteFailed,
                          "Unable to replace project file: " + error.message(),
                          path)};
    }
    removeStagingSilently(staging.path);
    static_cast<void>(flushToStorage(parent.empty() ? std::filesystem::path{"."} : parent));
#endif
    return {
        {},
        ProjectFileRevision{
            true,
            serialized,
        },
    };
}

ProjectFileLoadResult loadProjectFile(const std::filesystem::path& path)
{
    auto revision =
        captureProjectFileRevision(path);
    if (!revision.ok())
    {
        return {
            {},
            {*revision.error},
            {},
        };
    }
    if (!revision.revision->exists)
    {
        return {
            {},
            {makeError(
                ProjectFileErrorCode::ReadFailed,
                "Unable to open project file",
                path)},
            {},
        };
    }

    auto parsed =
        parseProjectDocument(
            revision.revision->contents);
    if (!parsed.ok())
    {
        ProjectFileLoadResult result;
        result.errors.reserve(parsed.errors.size());
        for (const auto& error : parsed.errors)
        {
            result.errors.push_back({
                ProjectFileErrorCode::InvalidProject,
                error.message,
                pathToUtf8(path) + ":" + error.path,
            });
        }
        return result;
    }

    return {
        std::move(parsed.document),
        {},
        std::move(revision.revision),
    };
}
}
