#include "talas/audio/AudioFilePublication.h"

#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

namespace talas::audio
{
namespace
{
#if !defined(_WIN32)
[[nodiscard]] bool sameRegularFileIdentity(
    int descriptor,
    int directory,
    const std::string& name) noexcept
{
    struct stat held{};
    struct stat named{};
    return descriptor >= 0
        && ::fstat(descriptor, &held) == 0
        && S_ISREG(held.st_mode)
        && ::fstatat(
               directory,
               name.c_str(),
               &named,
               AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(named.st_mode)
        && held.st_dev == named.st_dev
        && held.st_ino == named.st_ino;
}

[[nodiscard]] bool heldRegularFileMatchesBytes(
    int descriptor,
    std::span<const std::uint8_t> expected) noexcept
{
    struct stat before{};
    if (descriptor < 0 || ::fstat(descriptor, &before) != 0
        || !S_ISREG(before.st_mode) || before.st_size < 0
        || static_cast<std::uint64_t>(before.st_size) != expected.size())
    {
        return false;
    }
    std::uint8_t buffer[4096]{};
    std::size_t offset = 0U;
    while (offset < expected.size())
    {
        const auto requested = std::min(
            sizeof(buffer), expected.size() - offset);
        const auto count = ::pread(
            descriptor,
            buffer,
            requested,
            static_cast<off_t>(offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0
            || std::memcmp(
                   buffer,
                   expected.data() + offset,
                   static_cast<std::size_t>(count)) != 0)
        {
            return false;
        }
        offset += static_cast<std::size_t>(count);
    }
    struct stat after{};
    return ::fstat(descriptor, &after) == 0
        && before.st_dev == after.st_dev
        && before.st_ino == after.st_ino
        && before.st_size == after.st_size
        && before.st_mtime == after.st_mtime
        && before.st_ctime == after.st_ctime;
}
#endif

std::atomic<std::uint64_t> stagingSequence = 0;

#if !defined(_WIN32)
// Renames without replacing an existing entry. Volumes without an atomic
// no-replace rename, such as exFAT and FAT on macOS, fall back to a check
// followed by a plain rename.
[[nodiscard]] bool renameWithoutReplacing(
    int fromDirectory,
    const char* from,
    int toDirectory,
    const char* to) noexcept
{
#if defined(__APPLE__)
    if (::renameatx_np(fromDirectory, from, toDirectory, to, RENAME_EXCL) == 0)
        return true;
    if (errno != ENOTSUP && errno != EINVAL)
        return false;
#elif defined(__linux__)
    if (::syscall(
            SYS_renameat2,
            fromDirectory,
            from,
            toDirectory,
            to,
            RENAME_NOREPLACE) == 0)
    {
        return true;
    }
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
        return false;
#endif
    struct stat existing{};
    if (::fstatat(toDirectory, to, &existing, AT_SYMLINK_NOFOLLOW) == 0)
    {
        errno = EEXIST;
        return false;
    }
    if (errno != ENOENT)
        return false;
    return ::renameat(fromDirectory, from, toDirectory, to) == 0;
}

// Exchanges two entries. Volumes without an atomic exchange fall back to
// three renames through a scratch name, undone when a step fails.
[[nodiscard]] bool exchangeEntries(
    int firstDirectory,
    const char* first,
    int secondDirectory,
    const char* second,
    int scratchDirectory,
    const char* scratch) noexcept
{
#if defined(__APPLE__)
    if (::renameatx_np(
            firstDirectory, first, secondDirectory, second, RENAME_SWAP) == 0)
        return true;
    if (errno != ENOTSUP && errno != EINVAL)
        return false;
#elif defined(__linux__)
    if (::syscall(
            SYS_renameat2,
            firstDirectory,
            first,
            secondDirectory,
            second,
            RENAME_EXCHANGE) == 0)
    {
        return true;
    }
    if (errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
        return false;
#endif
    if (!renameWithoutReplacing(
            secondDirectory, second, scratchDirectory, scratch))
        return false;
    if (!renameWithoutReplacing(
            firstDirectory, first, secondDirectory, second))
    {
        const auto failure = errno;
        (void) renameWithoutReplacing(
            scratchDirectory, scratch, secondDirectory, second);
        errno = failure;
        return false;
    }
    if (!renameWithoutReplacing(
            scratchDirectory, scratch, firstDirectory, first))
    {
        const auto failure = errno;
        if (renameWithoutReplacing(
                secondDirectory, second, firstDirectory, first))
        {
            (void) renameWithoutReplacing(
                scratchDirectory, scratch, secondDirectory, second);
        }
        errno = failure;
        return false;
    }
    return true;
}

[[nodiscard]] bool quarantineAndRemoveHeldEntry(
    int descriptor,
    int parent,
    const std::string& name,
    bool directory,
    std::string& error) noexcept
{
    try
    {
        if (descriptor < 0 || parent < 0 || name.empty())
        {
            error = "Private audio cleanup authority is unavailable";
            return false;
        }
        const auto sameIdentity = [&](const char* candidate) noexcept {
            struct stat held{};
            struct stat named{};
            if (::fstat(descriptor, &held) != 0
                || ::fstatat(
                    parent,
                    candidate,
                    &named,
                    AT_SYMLINK_NOFOLLOW) != 0)
            {
                return false;
            }
            return directory
                ? S_ISDIR(held.st_mode) && S_ISDIR(named.st_mode)
                    && held.st_dev == named.st_dev
                    && held.st_ino == named.st_ino
                : S_ISREG(held.st_mode) && S_ISREG(named.st_mode)
                    && held.st_dev == named.st_dev
                    && held.st_ino == named.st_ino;
        };
        const auto renameExclusive = [&](
            const char* source,
            const char* destination) noexcept {
            return renameWithoutReplacing(
                parent, source, parent, destination);
        };

        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        char quarantineName[96]{};
        bool moved = false;
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            const auto sequence = stagingSequence.fetch_add(
                1, std::memory_order_relaxed);
            const auto written = std::snprintf(
                quarantineName,
                sizeof(quarantineName),
                ".talas-audio-cleanup-%lld-%llu",
                static_cast<long long>(stamp),
                static_cast<unsigned long long>(sequence));
            if (written <= 0
                || static_cast<std::size_t>(written) >= sizeof(quarantineName))
            {
                errno = ENAMETOOLONG;
                break;
            }
            if (renameExclusive(name.c_str(), quarantineName))
            {
                moved = true;
                break;
            }
            if (errno != EEXIST)
                break;
        }
        if (!moved)
        {
            error = "Unable to quarantine private audio cleanup entry: "
                + std::error_code(errno, std::generic_category()).message();
            return false;
        }
        if (!sameIdentity(quarantineName))
        {
            const auto restored = renameExclusive(
                quarantineName, name.c_str());
            error = restored
                ? "Private audio cleanup entry identity changed"
                : "Private audio cleanup entry changed and restoration failed";
            return false;
        }
        if (::unlinkat(
                parent,
                quarantineName,
                directory ? AT_REMOVEDIR : 0) == 0)
        {
            return true;
        }
        const auto removeError = errno;
        const auto restored = renameExclusive(
            quarantineName, name.c_str());
        error = restored
            ? "Unable to remove quarantined private audio cleanup entry: "
                + std::error_code(
                    removeError, std::generic_category()).message()
            : "Unable to remove or restore quarantined private audio cleanup entry";
        return false;
    }
    catch (...)
    {
        try
        {
            error = "Private audio cleanup failed";
        }
        catch (...)
        {
        }
        return false;
    }
}
#endif

#if defined(_WIN32)
[[nodiscard]] std::filesystem::path createStagingDirectory(
    const std::filesystem::path& destination,
    std::string& stagingName,
    std::string& error)
{
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    for (std::size_t attempt = 0; attempt < 64; ++attempt)
    {
        auto candidate = destination;
        candidate += ".talas-take-staging-"
            + std::to_string(stamp) + "-"
            + std::to_string(
                stagingSequence.fetch_add(1, std::memory_order_relaxed));
        stagingName = talas::project::pathToUtf8(candidate.filename());
        std::error_code createError;
        if (std::filesystem::create_directory(candidate, createError))
            return candidate;
        if (createError
            && createError != std::errc::file_exists)
        {
            error = "Unable to create private audio staging directory: "
                + createError.message();
            return {};
        }
    }
    error = "Unable to reserve a unique private audio staging directory";
    return {};
}
#endif

[[nodiscard]] std::filesystem::path comparablePath(
    const std::filesystem::path& path)
{
    std::error_code absoluteError;
    auto result = path.is_absolute()
        ? path
        : std::filesystem::absolute(path, absoluteError);
    if (absoluteError)
        result = path;
    result = result.lexically_normal();
    std::error_code canonicalError;
    const auto canonical = std::filesystem::weakly_canonical(
        result,
        canonicalError);
    return canonicalError ? result : canonical;
}

[[nodiscard]] bool pathIsInside(
    const std::filesystem::path& path,
    const std::filesystem::path& directory)
{
    const auto relative = path.lexically_relative(directory);
    if (relative.empty())
        return path == directory;
    return *relative.begin() != "..";
}

[[nodiscard]] bool pathsAlias(
    const std::filesystem::path& left,
    const std::filesystem::path& right)
{
    if (left == right)
        return true;
    std::error_code equivalentError;
    const auto equivalent = std::filesystem::equivalent(
        left,
        right,
        equivalentError);
    return !equivalentError && equivalent;
}
}

struct AudioFilePublication::Impl
{
    std::string destinationName;
    std::string stagingName;
    std::string temporaryName;
#if defined(_WIN32)
    std::vector<HANDLE> ancestors;
    HANDLE parent = INVALID_HANDLE_VALUE;
    HANDLE staging = INVALID_HANDLE_VALUE;
    HANDLE temporary = INVALID_HANDLE_VALUE;
#else
    int parent = -1;
    int staging = -1;
    int temporary = -1;
    int stagedTemporary = -1;
#endif
    bool publicationInstalled = false;
    std::vector<std::uint8_t> verifiedTemporaryBytes;
    std::vector<std::uint8_t> verifiedDestinationBytes;
    bool temporaryBytesVerified = false;
    bool destinationBytesVerified = false;

    ~Impl()
    {
#if defined(_WIN32)
        if (temporary != INVALID_HANDLE_VALUE)
        {
            if (!publicationInstalled)
            {
                FILE_DISPOSITION_INFO disposition{TRUE};
                (void) ::SetFileInformationByHandle(
                    temporary,
                    FileDispositionInfo,
                    &disposition,
                    sizeof(disposition));
            }
            ::CloseHandle(temporary);
        }
        if (staging != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            (void) ::SetFileInformationByHandle(
                staging,
                FileDispositionInfo,
                &disposition,
                sizeof(disposition));
            ::CloseHandle(staging);
        }
        if (parent != INVALID_HANDLE_VALUE)
            ::CloseHandle(parent);
        for (auto iterator = ancestors.rbegin();
             iterator != ancestors.rend();
             ++iterator)
        {
            if (*iterator != INVALID_HANDLE_VALUE)
                ::CloseHandle(*iterator);
        }
#else
        const auto cleanupTemporary = stagedTemporary >= 0
            ? stagedTemporary
            : (!publicationInstalled ? temporary : -1);
        if (staging >= 0 && cleanupTemporary >= 0)
        {
            std::string ignored;
            (void) quarantineAndRemoveHeldEntry(
                cleanupTemporary,
                staging,
                temporaryName,
                false,
                ignored);
        }
        if (temporary >= 0)
            ::close(temporary);
        if (stagedTemporary >= 0)
            ::close(stagedTemporary);
        if (staging >= 0 && parent >= 0 && !stagingName.empty())
        {
            std::string ignored;
            (void) quarantineAndRemoveHeldEntry(
                staging,
                parent,
                stagingName,
                true,
                ignored);
        }
        if (staging >= 0)
            ::close(staging);
        if (parent >= 0)
            ::close(parent);
#endif
    }
};

bool audioFileDestinationConflicts(
    const std::filesystem::path& destination,
    std::span<const std::filesystem::path> protectedFiles,
    std::span<const std::filesystem::path> protectedDirectories) noexcept
{
    try
    {
        if (destination.empty())
            return true;
        const auto candidate = comparablePath(destination);
        for (const auto& file : protectedFiles)
        {
            if (!file.empty()
                && pathsAlias(candidate, comparablePath(file)))
                return true;
        }
        for (const auto& directory : protectedDirectories)
        {
            if (!directory.empty()
                && pathIsInside(candidate, comparablePath(directory)))
            {
                return true;
            }
        }
        return false;
    }
    catch (...)
    {
        return true;
    }
}

AudioFilePublication::AudioFilePublication(
    std::filesystem::path destination,
    std::filesystem::path stagingDirectory,
    std::filesystem::path temporaryPath,
    ExistingAudioFilePolicy existingPolicy,
    std::unique_ptr<Impl> impl)
    : destination_(std::move(destination))
    , stagingDirectory_(std::move(stagingDirectory))
    , temporaryPath_(std::move(temporaryPath))
    , existingPolicy_(existingPolicy)
    , impl_(std::move(impl))
{
}

std::unique_ptr<AudioFilePublication> AudioFilePublication::prepare(
    const std::filesystem::path& destination,
    ExistingAudioFilePolicy existingPolicy,
    std::string& error) noexcept
{
    try
    {
        error.clear();
        if (destination.empty() || destination.filename().empty()
            || destination.filename() == "."
            || destination.filename() == "..")
        {
            error = "Audio destination must name one file";
            return nullptr;
        }

        std::error_code absoluteError;
        const auto absoluteDestination =
            std::filesystem::absolute(destination, absoluteError)
                .lexically_normal();
        if (absoluteError)
        {
            error = "Unable to resolve audio destination: "
                + absoluteError.message();
            return nullptr;
        }
        std::error_code parentError;
        const auto parent = std::filesystem::weakly_canonical(
            absoluteDestination.parent_path(), parentError);
        if (parentError || parent.empty())
        {
            error = "Audio destination parent must be an existing directory";
            if (parentError)
                error += ": " + parentError.message();
            return nullptr;
        }
        const auto resolvedDestination =
            parent / absoluteDestination.filename();
        auto impl = std::make_unique<Impl>();
        impl->destinationName =
            talas::project::pathToUtf8(resolvedDestination.filename());
        impl->temporaryName = impl->destinationName;

#if defined(_WIN32)
        impl->parent = ::CreateFileW(
            parent.c_str(),
            FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD
                | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (impl->parent == INVALID_HANDLE_VALUE)
        {
            error = "Unable to hold audio destination directory authority";
            return nullptr;
        }
        std::error_code destinationError;
        const auto destinationStatus = statusWithoutMissingError(
            resolvedDestination,
            destinationError);
        if (destinationError)
        {
            error = "Unable to inspect audio destination: "
                + destinationError.message();
            return nullptr;
        }
        if (destinationStatus.type()
                != std::filesystem::file_type::not_found
            && !std::filesystem::is_regular_file(destinationStatus))
        {
            error = "Existing audio destination must be a regular file";
            return nullptr;
        }
        if (destinationStatus.type()
                != std::filesystem::file_type::not_found
            && existingPolicy == ExistingAudioFilePolicy::Reject)
        {
            error = "Audio destination already exists; explicit replacement is required";
            return nullptr;
        }
        auto staging = createStagingDirectory(
            resolvedDestination, impl->stagingName, error);
        if (staging.empty())
            return nullptr;
        impl->staging = ::CreateFileW(
            staging.c_str(),
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (impl->staging == INVALID_HANDLE_VALUE)
        {
            const auto cleanupCode = ::RemoveDirectoryW(staging.c_str())
                ? ERROR_SUCCESS
                : ::GetLastError();
            error = "Unable to hold private audio staging directory authority";
            if (cleanupCode != ERROR_SUCCESS)
            {
                error += "; unable to remove the unopened staging directory: "
                    + std::error_code(
                        static_cast<int>(cleanupCode),
                        std::system_category()).message();
            }
            return nullptr;
        }
#else
        impl->parent = ::open(
            parent.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (impl->parent < 0)
        {
            error = "Unable to hold audio destination directory authority: "
                + std::error_code(errno, std::generic_category()).message();
            return nullptr;
        }
        struct stat destinationStatus{};
        const auto destinationResult = ::fstatat(
            impl->parent,
            impl->destinationName.c_str(),
            &destinationStatus,
            AT_SYMLINK_NOFOLLOW);
        const auto destinationExists = destinationResult == 0;
        if (destinationResult != 0 && errno != ENOENT)
        {
            error = "Unable to inspect audio destination: "
                + std::error_code(errno, std::generic_category()).message();
            return nullptr;
        }
        if (destinationExists && !S_ISREG(destinationStatus.st_mode))
        {
            error = "Existing audio destination must be a regular file";
            return nullptr;
        }
        if (destinationExists
            && existingPolicy == ExistingAudioFilePolicy::Reject)
        {
            error = "Audio destination already exists; explicit replacement is required";
            return nullptr;
        }

        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        bool created = false;
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            impl->stagingName = ".talas-take-staging-"
                + std::to_string(stamp) + "-"
                + std::to_string(stagingSequence.fetch_add(
                    1, std::memory_order_relaxed));
            if (::mkdirat(impl->parent, impl->stagingName.c_str(), 0700) == 0)
            {
                created = true;
                break;
            }
            if (errno != EEXIST)
            {
                error = "Unable to create private audio staging directory: "
                    + std::error_code(errno, std::generic_category()).message();
                return nullptr;
            }
        }
        if (!created)
        {
            error = "Unable to reserve a unique private audio staging directory";
            return nullptr;
        }
        impl->staging = ::openat(
            impl->parent,
            impl->stagingName.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (impl->staging < 0)
        {
            const auto openError = errno;
            const auto removed = ::unlinkat(
                impl->parent,
                impl->stagingName.c_str(),
                AT_REMOVEDIR) == 0;
            error = "Unable to hold private audio staging directory authority";
            if (!removed)
            {
                error += "; unable to remove the unopened staging directory: "
                    + std::error_code(
                        errno, std::generic_category()).message();
            }
            else
            {
                errno = openError;
            }
            return nullptr;
        }
        const auto staging = parent / talas::project::pathFromUtf8(
            impl->stagingName);
#endif
        auto temporary = staging / resolvedDestination.filename();
        auto publication = std::unique_ptr<AudioFilePublication>{
            new AudioFilePublication{
                resolvedDestination,
                staging,
                std::move(temporary),
                existingPolicy,
                std::move(impl)}};
        return publication;
    }
    catch (const std::exception& exception)
    {
        try
        {
            error = "Unable to prepare private audio publication: ";
            error += exception.what();
        }
        catch (...)
        {
        }
        return nullptr;
    }
    catch (...)
    {
        try
        {
            error = "Unable to prepare private audio publication";
        }
        catch (...)
        {
        }
        return nullptr;
    }
}

std::unique_ptr<AudioFilePublication> AudioFilePublication::prepareContained(
    const std::filesystem::path& rootDirectory,
    const std::filesystem::path& relativeDestination,
    ExistingAudioFilePolicy existingPolicy,
    std::string& error) noexcept
{
    try
    {
        error.clear();
        if (rootDirectory.empty() || relativeDestination.empty()
            || relativeDestination.is_absolute()
            || relativeDestination.has_root_name()
            || relativeDestination.has_root_directory())
        {
            error = "Contained audio destination must be relative to one root";
            return nullptr;
        }
        for (const auto& component : relativeDestination)
        {
            if (component.empty() || component == "." || component == "..")
            {
                error = "Contained audio destination has an invalid path component";
                return nullptr;
            }
        }
        if (relativeDestination.filename().empty())
        {
            error = "Contained audio destination must name one file";
            return nullptr;
        }
#if defined(_WIN32)
        std::error_code absoluteError;
        const auto root = std::filesystem::absolute(
            rootDirectory, absoluteError).lexically_normal();
        if (absoluteError || root.empty() || !root.is_absolute())
        {
            error = "Unable to resolve contained audio root authority";
            return nullptr;
        }
        auto impl = std::make_unique<Impl>();
        const auto openDirectory = [](
            const std::filesystem::path& path,
            DWORD access) -> HANDLE {
            const auto handle = ::CreateFileW(
                path.c_str(),
                access,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return handle;
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            if (!::GetFileInformationByHandleEx(
                    handle,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes))
                || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
                || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            {
                ::CloseHandle(handle);
                return INVALID_HANDLE_VALUE;
            }
            return handle;
        };
        auto currentPath = root.root_path();
        auto current = openDirectory(
            currentPath, FILE_READ_ATTRIBUTES);
        if (current == INVALID_HANDLE_VALUE)
        {
            error = "Unable to hold contained audio volume authority";
            return nullptr;
        }
        for (const auto& component : root.relative_path())
        {
            currentPath /= component;
            const auto next = openDirectory(
                currentPath, FILE_READ_ATTRIBUTES);
            if (next == INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(current);
                error = "Contained audio root contains an indirect or unavailable component";
                return nullptr;
            }
            try
            {
                impl->ancestors.push_back(current);
            }
            catch (...)
            {
                ::CloseHandle(current);
                ::CloseHandle(next);
                throw;
            }
            current = next;
        }
        for (const auto& component : relativeDestination.parent_path())
        {
            currentPath /= component;
            if (!::CreateDirectoryW(currentPath.c_str(), nullptr)
                && ::GetLastError() != ERROR_ALREADY_EXISTS)
            {
                ::CloseHandle(current);
                error = "Unable to create contained audio directory";
                return nullptr;
            }
            const auto next = openDirectory(
                currentPath, FILE_READ_ATTRIBUTES);
            if (next == INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(current);
                error = "Contained audio path is indirect or unavailable";
                return nullptr;
            }
            try
            {
                impl->ancestors.push_back(current);
            }
            catch (...)
            {
                ::CloseHandle(current);
                ::CloseHandle(next);
                throw;
            }
            current = next;
        }
        // Reopen the exact final directory with publication rights while the
        // first handle still prevents replacement of that directory object.
        const auto publicationParent = openDirectory(
            currentPath,
            FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD
                | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE);
        if (publicationParent == INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(current);
            error = "Unable to hold contained audio parent authority";
            return nullptr;
        }
        ::CloseHandle(current);
        impl->parent = publicationParent;
        impl->destinationName = talas::project::pathToUtf8(
            relativeDestination.filename());
        impl->temporaryName = impl->destinationName;
        const auto destination = currentPath / relativeDestination.filename();
        const auto existing = ::CreateFileW(
            destination.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (existing != INVALID_HANDLE_VALUE)
        {
            FILE_ATTRIBUTE_TAG_INFO attributes{};
            const auto regular = ::GetFileInformationByHandleEx(
                    existing,
                    FileAttributeTagInfo,
                    &attributes,
                    sizeof(attributes))
                && (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0U
                && (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
            ::CloseHandle(existing);
            if (!regular)
            {
                error = "Existing contained audio destination must be a regular file";
                return nullptr;
            }
            if (existingPolicy == ExistingAudioFilePolicy::Reject)
            {
                error = "Audio destination already exists; explicit replacement is required";
                return nullptr;
            }
        }
        else if (::GetLastError() != ERROR_FILE_NOT_FOUND
                 && ::GetLastError() != ERROR_PATH_NOT_FOUND)
        {
            error = "Unable to inspect contained audio destination";
            return nullptr;
        }
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        std::filesystem::path stagingDirectory;
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            impl->stagingName = ".talas-take-staging-"
                + std::to_string(stamp) + "-"
                + std::to_string(stagingSequence.fetch_add(
                    1, std::memory_order_relaxed));
            stagingDirectory = currentPath
                / talas::project::pathFromUtf8(impl->stagingName);
            if (::CreateDirectoryW(stagingDirectory.c_str(), nullptr))
                break;
            if (::GetLastError() != ERROR_ALREADY_EXISTS)
            {
                error = "Unable to create private audio staging directory";
                return nullptr;
            }
            stagingDirectory.clear();
        }
        if (stagingDirectory.empty())
        {
            error = "Unable to reserve a unique private audio staging directory";
            return nullptr;
        }
        impl->staging = openDirectory(
            stagingDirectory,
            FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE);
        if (impl->staging == INVALID_HANDLE_VALUE)
        {
            const auto cleanupCode = ::RemoveDirectoryW(
                stagingDirectory.c_str())
                ? ERROR_SUCCESS
                : ::GetLastError();
            error = "Unable to hold private audio staging directory authority";
            if (cleanupCode != ERROR_SUCCESS)
            {
                error += "; unable to remove the unopened staging directory: "
                    + std::error_code(
                        static_cast<int>(cleanupCode),
                        std::system_category()).message();
            }
            return nullptr;
        }
        return std::unique_ptr<AudioFilePublication>{
            new AudioFilePublication{
                destination,
                stagingDirectory,
                stagingDirectory / destination.filename(),
                existingPolicy,
                std::move(impl)}};
#else
        std::error_code rootError;
        const auto root = std::filesystem::absolute(
            rootDirectory, rootError).lexically_normal();
        if (rootError || root.empty() || !root.is_absolute())
        {
            error = "Unable to resolve contained audio root authority";
            return nullptr;
        }
        const auto rootRelative = root.relative_path();
        auto rootComponent = rootRelative.begin();
        const auto rootEnd = rootRelative.end();
        std::filesystem::path trustedTop = root.root_path();
        if (rootComponent != rootEnd)
        {
            trustedTop /= *rootComponent;
            std::error_code topError;
            trustedTop = std::filesystem::weakly_canonical(
                trustedTop, topError);
            if (topError)
            {
                error = "Unable to resolve contained audio volume authority";
                return nullptr;
            }
            ++rootComponent;
        }
        int current = ::open(
            trustedTop.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (current < 0)
        {
            error = "Unable to hold contained audio root authority: "
                + std::error_code(errno, std::generic_category()).message();
            return nullptr;
        }
        struct DescriptorGuard
        {
            int value;
            ~DescriptorGuard() { if (value >= 0) ::close(value); }
        } currentGuard{current};
        for (; rootComponent != rootEnd; ++rootComponent)
        {
            const auto next = ::openat(
                currentGuard.value,
                rootComponent->c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0)
            {
                error = "Contained audio root is unavailable or indirect: "
                    + std::error_code(errno, std::generic_category()).message();
                return nullptr;
            }
            ::close(currentGuard.value);
            currentGuard.value = next;
        }
        for (const auto& component : relativeDestination.parent_path())
        {
            auto next = ::openat(
                currentGuard.value,
                component.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0 && errno == ENOENT)
            {
                if (::mkdirat(currentGuard.value, component.c_str(), 0700) != 0
                    && errno != EEXIST)
                {
                    error = "Unable to create contained audio directory: "
                        + std::error_code(errno, std::generic_category()).message();
                    return nullptr;
                }
                next = ::openat(
                    currentGuard.value,
                    component.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            }
            if (next < 0)
            {
                error = "Contained audio path is unavailable or indirect: "
                    + std::error_code(errno, std::generic_category()).message();
                return nullptr;
            }
            ::close(currentGuard.value);
            currentGuard.value = next;
        }

        auto impl = std::make_unique<Impl>();
        impl->parent = currentGuard.value;
        currentGuard.value = -1;
        impl->destinationName = talas::project::pathToUtf8(
            relativeDestination.filename());
        impl->temporaryName = impl->destinationName;
        struct stat destinationStatus{};
        const auto destinationResult = ::fstatat(
            impl->parent,
            impl->destinationName.c_str(),
            &destinationStatus,
            AT_SYMLINK_NOFOLLOW);
        if (destinationResult != 0 && errno != ENOENT)
        {
            error = "Unable to inspect contained audio destination";
            return nullptr;
        }
        if (destinationResult == 0 && !S_ISREG(destinationStatus.st_mode))
        {
            error = "Existing contained audio destination must be a regular file";
            return nullptr;
        }
        if (destinationResult == 0
            && existingPolicy == ExistingAudioFilePolicy::Reject)
        {
            error = "Audio destination already exists; explicit replacement is required";
            return nullptr;
        }
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        bool created = false;
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            impl->stagingName = ".talas-take-staging-"
                + std::to_string(stamp) + "-"
                + std::to_string(stagingSequence.fetch_add(
                    1, std::memory_order_relaxed));
            if (::mkdirat(impl->parent, impl->stagingName.c_str(), 0700) == 0)
            {
                created = true;
                break;
            }
            if (errno != EEXIST)
            {
                error = "Unable to create private audio staging directory";
                return nullptr;
            }
        }
        if (!created)
        {
            error = "Unable to reserve a unique private audio staging directory";
            return nullptr;
        }
        impl->staging = ::openat(
            impl->parent,
            impl->stagingName.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (impl->staging < 0)
        {
            const auto openError = errno;
            const auto removed = ::unlinkat(
                impl->parent,
                impl->stagingName.c_str(),
                AT_REMOVEDIR) == 0;
            error = "Unable to hold private audio staging directory authority";
            if (!removed)
            {
                error += "; unable to remove the unopened staging directory: "
                    + std::error_code(
                        errno, std::generic_category()).message();
            }
            else
            {
                errno = openError;
            }
            return nullptr;
        }
        const auto destination = root / relativeDestination;
        const auto stagingDirectory = destination.parent_path()
            / talas::project::pathFromUtf8(impl->stagingName);
        return std::unique_ptr<AudioFilePublication>{
            new AudioFilePublication{
                destination,
                stagingDirectory,
                stagingDirectory / destination.filename(),
                existingPolicy,
                std::move(impl)}};
#endif
    }
    catch (...)
    {
        try
        {
            error = "Unable to prepare contained audio publication";
        }
        catch (...)
        {
        }
        return nullptr;
    }
}

AudioFilePublication::~AudioFilePublication()
{
    if (published_)
        return;
    std::string ignored;
    (void)discard(ignored);
}

const std::filesystem::path& AudioFilePublication::destination() const noexcept
{
    return destination_;
}

const std::filesystem::path& AudioFilePublication::temporaryPath() const noexcept
{
    return temporaryPath_;
}

std::FILE* AudioFilePublication::openTemporaryFile(
    std::string& error) noexcept
{
    try
    {
        error.clear();
        if (impl_ == nullptr || published_)
        {
            error = "Private audio publication is unavailable";
            return nullptr;
        }
#if defined(_WIN32)
        if (impl_->temporary != INVALID_HANDLE_VALUE)
        {
            error = "Private audio staging file is already open";
            return nullptr;
        }
        const auto handle = ::CreateFileW(
            temporaryPath_.c_str(),
            GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "Unable to create exact private audio staging file";
            return nullptr;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!::GetFileInformationByHandleEx(
                handle,
                FileAttributeTagInfo,
                &attributes,
                sizeof(attributes))
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
        {
            ::CloseHandle(handle);
            error = "Private audio staging file is indirect or invalid";
            return nullptr;
        }
        if (!::DuplicateHandle(
                ::GetCurrentProcess(),
                handle,
                ::GetCurrentProcess(),
                &impl_->temporary,
                0,
                FALSE,
                DUPLICATE_SAME_ACCESS))
        {
            ::CloseHandle(handle);
            error = "Unable to retain exact private audio staging identity";
            return nullptr;
        }
        const auto descriptor = ::_open_osfhandle(
            reinterpret_cast<intptr_t>(handle), _O_BINARY);
        if (descriptor < 0)
        {
            ::CloseHandle(impl_->temporary);
            impl_->temporary = INVALID_HANDLE_VALUE;
            ::CloseHandle(handle);
            error = "Unable to bind private audio staging file";
            return nullptr;
        }
        auto* output = ::_fdopen(descriptor, "wb");
        if (output == nullptr)
        {
            ::CloseHandle(impl_->temporary);
            impl_->temporary = INVALID_HANDLE_VALUE;
            ::_close(descriptor);
            error = "Unable to open private audio staging stream";
            return nullptr;
        }
        return output;
#else
        if (impl_->temporary >= 0)
        {
            error = "Private audio staging file is already open";
            return nullptr;
        }
        const auto descriptor = ::openat(
            impl_->staging,
            impl_->temporaryName.c_str(),
            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600);
        if (descriptor < 0)
        {
            error = "Unable to create exact private audio staging file: "
                + std::error_code(errno, std::generic_category()).message();
            return nullptr;
        }
        impl_->temporary = ::fcntl(
            descriptor, F_DUPFD_CLOEXEC, 0);
        if (impl_->temporary < 0)
        {
            ::close(descriptor);
            (void) ::unlinkat(
                impl_->staging, impl_->temporaryName.c_str(), 0);
            error = "Unable to retain exact private audio staging identity";
            return nullptr;
        }
        auto* output = ::fdopen(descriptor, "wb");
        if (output == nullptr)
        {
            ::close(impl_->temporary);
            impl_->temporary = -1;
            ::close(descriptor);
            error = "Unable to open private audio staging stream";
            return nullptr;
        }
        return output;
#endif
    }
    catch (...)
    {
        try
        {
            error = "Unable to create private audio staging stream";
        }
        catch (...)
        {
        }
        return nullptr;
    }
}

namespace
{
[[nodiscard]] bool readHeldPublicationFile(
#if defined(_WIN32)
    HANDLE handle,
#else
    int descriptor,
#endif
    std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes,
    std::string& error) noexcept
{
    try
    {
        bytes.clear();
#if defined(_WIN32)
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "Held audio file identity is unavailable";
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER size{};
        if (!::GetFileInformationByHandleEx(
                handle, FileAttributeTagInfo, &attributes, sizeof(attributes))
            || (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
            || !::GetFileSizeEx(handle, &size) || size.QuadPart < 0
            || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
        {
            error = "Held audio file is invalid or exceeds its byte bound";
            return false;
        }
        LARGE_INTEGER zero{};
        if (!::SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN))
        {
            error = "Unable to rewind held audio file";
            return false;
        }
        bytes.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            DWORD read = 0;
            const auto count = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - offset,
                std::numeric_limits<DWORD>::max()));
            if (!::ReadFile(handle, bytes.data() + offset, count, &read, nullptr)
                || read == 0U)
            {
                error = "Unable to read held audio file";
                return false;
            }
            offset += read;
        }
        std::uint8_t extra = 0;
        DWORD extraRead = 0;
        LARGE_INTEGER finalSize{};
        if (!::ReadFile(handle, &extra, 1, &extraRead, nullptr)
            || extraRead != 0U
            || !::GetFileSizeEx(handle, &finalSize)
            || finalSize.QuadPart != size.QuadPart)
        {
            error = "Held audio file changed while reading";
            return false;
        }
#else
        if (descriptor < 0)
        {
            error = "Held audio file identity is unavailable";
            return false;
        }
        struct stat before{};
        if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode)
            || before.st_size < 0
            || static_cast<std::uint64_t>(before.st_size) > maximumBytes)
        {
            error = "Held audio file is invalid or exceeds its byte bound";
            return false;
        }
        bytes.resize(static_cast<std::size_t>(before.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            const auto count = ::pread(
                descriptor,
                bytes.data() + offset,
                bytes.size() - offset,
                static_cast<off_t>(offset));
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
            {
                error = "Unable to read held audio file";
                return false;
            }
            offset += static_cast<std::size_t>(count);
        }
        std::uint8_t extra = 0;
        ssize_t extraCount = 0;
        do
        {
            extraCount = ::pread(
                descriptor,
                &extra,
                1,
                static_cast<off_t>(bytes.size()));
        } while (extraCount < 0 && errno == EINTR);
        if (extraCount != 0)
        {
            error = "Held audio file changed while reading";
            return false;
        }
        struct stat after{};
        if (::fstat(descriptor, &after) != 0
            || before.st_dev != after.st_dev || before.st_ino != after.st_ino
            || before.st_size != after.st_size)
        {
            error = "Held audio file changed while reading";
            return false;
        }
#endif
        error.clear();
        return true;
    }
    catch (...)
    {
        try
        {
            error = "Unable to snapshot held audio file";
        }
        catch (...)
        {
        }
        return false;
    }
}
}

bool AudioFilePublication::readTemporaryFile(
    std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes,
    std::string& error) const noexcept
{
    try
    {
        if (impl_ == nullptr)
            return false;
#if defined(_WIN32)
        if (impl_->temporary == INVALID_HANDLE_VALUE)
        {
            impl_->temporary = ::CreateFileW(
                temporaryPath_.c_str(),
                GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
        }
#else
        if (impl_->temporary < 0)
        {
            impl_->temporary = ::openat(
                impl_->staging,
                impl_->temporaryName.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        }
#endif
        const auto read = readHeldPublicationFile(
#if defined(_WIN32)
            impl_->temporary,
#else
            impl_->temporary,
#endif
            maximumBytes, bytes, error);
        if (read)
        {
            impl_->verifiedTemporaryBytes = bytes;
            impl_->temporaryBytesVerified = true;
        }
        return read;
    }
    catch (...)
    {
        bytes.clear();
        try
        {
            error = "Unable to retain verified private audio bytes";
        }
        catch (...)
        {
        }
        return false;
    }
}

bool AudioFilePublication::readDestinationFile(
    std::size_t maximumBytes,
    std::vector<std::uint8_t>& bytes,
    std::string& error) const noexcept
{
    try
    {
        if (impl_ == nullptr || !published_)
            return false;
        const auto read = readHeldPublicationFile(
#if defined(_WIN32)
            impl_->temporary,
#else
            impl_->temporary,
#endif
            maximumBytes, bytes, error);
        if (read)
        {
            impl_->verifiedDestinationBytes = bytes;
            impl_->destinationBytesVerified = true;
        }
        return read;
    }
    catch (...)
    {
        bytes.clear();
        try
        {
            error = "Unable to retain verified published audio bytes";
        }
        catch (...)
        {
        }
        return false;
    }
}

bool AudioFilePublication::publish(std::string& error)
{
    error.clear();
    if (published_ || impl_ == nullptr)
    {
        error = "Audio file was already published";
        return false;
    }
#if defined(_WIN32)
    if (impl_->temporary == INVALID_HANDLE_VALUE)
    {
        impl_->temporary = ::CreateFileW(
            temporaryPath_.c_str(),
            GENERIC_READ | DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
    }
    if (impl_->temporary == INVALID_HANDLE_VALUE)
    {
        error = "Private audio staging file is missing or invalid";
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO sourceAttributes{};
    if (!::GetFileInformationByHandleEx(
            impl_->temporary,
            FileAttributeTagInfo,
            &sourceAttributes,
            sizeof(sourceAttributes))
        || (sourceAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
        || (sourceAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
    {
        error = "Private audio staging file is indirect or invalid";
        return false;
    }
    const auto leaf = destination_.filename().native();
    const auto renameBytes = sizeof(FILE_RENAME_INFO)
        + leaf.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> renameStorage(renameBytes);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(
        renameStorage.data());
    rename->ReplaceIfExists =
        existingPolicy_ == ExistingAudioFilePolicy::Replace;
    rename->RootDirectory = impl_->parent;
    rename->FileNameLength = static_cast<DWORD>(
        leaf.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, leaf.data(), rename->FileNameLength);
    const auto moved = ::SetFileInformationByHandle(
        impl_->temporary,
        FileRenameInfo,
        rename,
        static_cast<DWORD>(renameBytes));
    const auto publishCode = ::GetLastError();
    if (!moved)
    {
        error = existingPolicy_ == ExistingAudioFilePolicy::Replace
            ? "Unable to atomically replace the existing audio destination: "
            : "Unable to publish audio file without overwriting a concurrent destination: ";
        error += std::error_code(
            static_cast<int>(publishCode), std::system_category()).message();
        return false;
    }
#else
    if (impl_->temporary < 0)
    {
        impl_->temporary = ::openat(
            impl_->staging,
            impl_->temporaryName.c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (!sameRegularFileIdentity(
            impl_->temporary,
            impl_->staging,
            impl_->temporaryName))
    {
        error = "Private audio staging leaf no longer names the verified file";
        return false;
    }
    struct stat destinationStatus{};
    const auto destinationResult = ::fstatat(
        impl_->parent,
        impl_->destinationName.c_str(),
        &destinationStatus,
        AT_SYMLINK_NOFOLLOW);
    if (destinationResult != 0 && errno != ENOENT)
    {
        error = "Unable to inspect audio destination";
        return false;
    }
    if (destinationResult == 0 && !S_ISREG(destinationStatus.st_mode))
    {
        error = "Existing audio destination must remain a regular file";
        return false;
    }
    if (destinationResult == 0
        && existingPolicy_ == ExistingAudioFilePolicy::Reject)
    {
        error = "Audio destination appeared before publication; no file was replaced";
        return false;
    }
    const auto destinationExisted = destinationResult == 0;
    // Scratch name for exchanges on volumes without an atomic exchange.
    const auto exchangeName = ".talas-exchange-"
        + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())
        + "-"
        + std::to_string(
            stagingSequence.fetch_add(1, std::memory_order_relaxed));
    bool exchanged = false;
    bool verifiedCopyInstallation = false;
    std::string verifiedCopyName;
    bool moved = false;
    if (impl_->temporaryBytesVerified)
    {
        int installed = -1;
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            verifiedCopyName = std::string{".talas-verified-"}
                + std::to_string(stamp) + "-"
                + std::to_string(stagingSequence.fetch_add(
                    1, std::memory_order_relaxed));
            installed = ::openat(
                impl_->staging,
                verifiedCopyName.c_str(),
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                0000);
            if (installed >= 0 || errno != EEXIST)
                break;
        }
        if (installed >= 0)
        {
            std::size_t offset = 0U;
            while (offset < impl_->verifiedTemporaryBytes.size())
            {
                const auto count = ::pwrite(
                    installed,
                    impl_->verifiedTemporaryBytes.data() + offset,
                    impl_->verifiedTemporaryBytes.size() - offset,
                    static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    break;
                offset += static_cast<std::size_t>(count);
            }
            const auto copyIsExact =
                offset == impl_->verifiedTemporaryBytes.size()
                && ::fsync(installed) == 0
                && ::fchmod(installed, 0400) == 0
                && sameRegularFileIdentity(
                    installed,
                    impl_->staging,
                    verifiedCopyName)
                && heldRegularFileMatchesBytes(
                    installed,
                    impl_->verifiedTemporaryBytes);
            if (copyIsExact)
            {
#if defined(__APPLE__) || defined(__linux__)
                if (existingPolicy_ == ExistingAudioFilePolicy::Replace
                    && destinationExisted)
                {
                    exchanged = true;
                    moved = exchangeEntries(
                        impl_->staging,
                        verifiedCopyName.c_str(),
                        impl_->parent,
                        impl_->destinationName.c_str(),
                        impl_->staging,
                        exchangeName.c_str());
                }
                else
                {
                    moved = renameWithoutReplacing(
                        impl_->staging,
                        verifiedCopyName.c_str(),
                        impl_->parent,
                        impl_->destinationName.c_str());
                }
#else
                if (existingPolicy_ == ExistingAudioFilePolicy::Replace)
                {
                    errno = ENOTSUP;
                }
                else
                {
                    moved = ::renameat(
                        impl_->staging,
                        verifiedCopyName.c_str(),
                        impl_->parent,
                        impl_->destinationName.c_str()) == 0;
                }
#endif
            }
            if (moved)
            {
                impl_->stagedTemporary = impl_->temporary;
                impl_->temporary = installed;
                verifiedCopyInstallation = true;
            }
            else
            {
                std::string cleanupError;
                (void) quarantineAndRemoveHeldEntry(
                    installed,
                    impl_->staging,
                    verifiedCopyName,
                    false,
                    cleanupError);
                ::close(installed);
            }
        }
    }
    else
    {
#if defined(__APPLE__) || defined(__linux__)
    if (existingPolicy_ == ExistingAudioFilePolicy::Replace
        && destinationExisted)
    {
        exchanged = true;
        moved = exchangeEntries(
            impl_->staging,
            impl_->temporaryName.c_str(),
            impl_->parent,
            impl_->destinationName.c_str(),
            impl_->staging,
            exchangeName.c_str());
    }
    else
    {
        moved = renameWithoutReplacing(
            impl_->staging,
            impl_->temporaryName.c_str(),
            impl_->parent,
            impl_->destinationName.c_str());
    }
#else
    if (existingPolicy_ == ExistingAudioFilePolicy::Replace)
    {
        errno = ENOTSUP;
        moved = false;
    }
    else
    {
        moved = ::linkat(
            impl_->staging,
            impl_->temporaryName.c_str(),
            impl_->parent,
            impl_->destinationName.c_str(),
            0) == 0;
    }
#endif
    }
    if (!moved)
    {
        error = existingPolicy_ == ExistingAudioFilePolicy::Replace
            ? "Unable to atomically replace the existing audio destination: "
            : "Unable to publish audio file without overwriting a concurrent destination: ";
        error += std::error_code(errno, std::generic_category()).message();
        return false;
    }
    const auto restoreInstalledSource = [&]() noexcept {
        if (verifiedCopyInstallation)
            return false;
        bool restored = false;
#if defined(__APPLE__) || defined(__linux__)
        restored = exchanged
            ? exchangeEntries(
                  impl_->staging,
                  impl_->temporaryName.c_str(),
                  impl_->parent,
                  impl_->destinationName.c_str(),
                  impl_->staging,
                  exchangeName.c_str())
            : renameWithoutReplacing(
                  impl_->parent,
                  impl_->destinationName.c_str(),
                  impl_->staging,
                  impl_->temporaryName.c_str());
#else
        if (!exchanged)
        {
            restored = ::renameat(
                impl_->parent,
                impl_->destinationName.c_str(),
                impl_->staging,
                impl_->temporaryName.c_str()) == 0;
        }
#endif
        return restored;
    };
    if (!verifiedCopyInstallation
        && !sameRegularFileIdentity(
            impl_->temporary,
            impl_->parent,
            impl_->destinationName))
    {
        const auto restored = restoreInstalledSource();
        error = restored
            ? "Audio publication source identity changed before installation"
            : "Audio publication source identity changed and fail-closed restoration failed";
        return false;
    }
    if (!verifiedCopyInstallation && impl_->temporaryBytesVerified)
    {
        std::vector<std::uint8_t> installedBytes;
        std::string verificationError;
        if (!readHeldPublicationFile(
                impl_->temporary,
                impl_->verifiedTemporaryBytes.size(),
                installedBytes,
                verificationError)
            || installedBytes != impl_->verifiedTemporaryBytes)
        {
            const auto restored = restoreInstalledSource();
            error = restored
                ? "Verified audio bytes changed before publication"
                : "Verified audio bytes changed and fail-closed restoration failed";
            return false;
        }
    }
#endif

    // Publication is now the irreversible truth. Cleanup failures retain a
    // diagnostic but cannot turn the already-installed artifact into Failed.
    published_ = true;
    impl_->publicationInstalled = true;
    try
    {
#if defined(_WIN32)
        if (impl_->staging != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            const auto removed = ::SetFileInformationByHandle(
                impl_->staging,
                FileDispositionInfo,
                &disposition,
                sizeof(disposition));
            const auto cleanupCode = ::GetLastError();
            if (!removed)
            {
                error = "Audio file was published, but staging-directory cleanup failed: "
                    + std::error_code(
                        static_cast<int>(cleanupCode),
                        std::system_category()).message();
                return true;
            }
            ::CloseHandle(impl_->staging);
            impl_->staging = INVALID_HANDLE_VALUE;
        }
#else
        if (verifiedCopyInstallation)
        {
            std::string cleanupError;
            if (!quarantineAndRemoveHeldEntry(
                    impl_->stagedTemporary,
                    impl_->staging,
                    impl_->temporaryName,
                    false,
                    cleanupError))
            {
                error = "Audio file was published, but staging-leaf cleanup failed: "
                    + cleanupError;
                return true;
            }
            ::close(impl_->stagedTemporary);
            impl_->stagedTemporary = -1;
        }
        if (exchanged)
        {
            const auto& displacedName = verifiedCopyInstallation
                ? verifiedCopyName
                : impl_->temporaryName;
            const auto displacedDestination = ::openat(
                impl_->staging,
                displacedName.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            std::string cleanupError;
            const auto removed = displacedDestination >= 0
                && quarantineAndRemoveHeldEntry(
                    displacedDestination,
                    impl_->staging,
                    displacedName,
                    false,
                    cleanupError);
            if (displacedDestination >= 0)
                ::close(displacedDestination);
            if (!removed)
            {
                error = "Audio file was published, but replaced-file cleanup failed";
                if (!cleanupError.empty())
                    error += ": " + cleanupError;
                return true;
            }
        }
        std::string directoryCleanupError;
        if (!quarantineAndRemoveHeldEntry(
            impl_->staging,
            impl_->parent,
            impl_->stagingName,
            true,
            directoryCleanupError))
        {
            error = "Audio file was published, but staging-directory cleanup failed: "
                + directoryCleanupError;
            return true;
        }
        if (impl_->staging >= 0)
        {
            ::close(impl_->staging);
            impl_->staging = -1;
        }
#endif
    }
    catch (...)
    {
        try
        {
            error = "Audio file was published, but staging cleanup failed";
        }
        catch (...)
        {
        }
    }
    return true;
}

bool AudioFilePublication::retract(std::string& error) noexcept
{
    try
    {
        error.clear();
        if (!published_ || impl_ == nullptr)
        {
            error = "Audio file is not retractable";
            return false;
        }
#if defined(_WIN32)
        if (impl_->temporary == INVALID_HANDLE_VALUE)
        {
            error = "Exact published audio identity is unavailable for retraction";
            return false;
        }
        if (impl_->destinationBytesVerified)
        {
            std::vector<std::uint8_t> currentBytes;
            std::string verificationError;
            if (!readHeldPublicationFile(
                    impl_->temporary,
                    impl_->verifiedDestinationBytes.size(),
                    currentBytes,
                    verificationError)
                || currentBytes != impl_->verifiedDestinationBytes)
            {
                error = "Published audio bytes changed before retraction";
                return false;
            }
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        const auto removed = ::SetFileInformationByHandle(
            impl_->temporary,
            FileDispositionInfo,
            &disposition,
            sizeof(disposition));
        if (!removed)
        {
            error = "Unable to retract exact published audio file";
            return false;
        }
        ::CloseHandle(impl_->temporary);
        impl_->temporary = INVALID_HANDLE_VALUE;
#else
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        std::string quarantineName;
        int quarantine = -1;
        for (std::size_t attempt = 0; attempt < 64; ++attempt)
        {
            quarantineName = ".talas-retract-"
                + std::to_string(stamp) + "-"
                + std::to_string(stagingSequence.fetch_add(
                    1, std::memory_order_relaxed));
            if (::mkdirat(
                    impl_->parent,
                    quarantineName.c_str(),
                    0700) == 0)
            {
                quarantine = ::openat(
                    impl_->parent,
                    quarantineName.c_str(),
                    O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
                if (quarantine >= 0)
                    break;
                (void) ::unlinkat(
                    impl_->parent,
                    quarantineName.c_str(),
                    AT_REMOVEDIR);
                quarantineName.clear();
                break;
            }
            if (errno != EEXIST)
                break;
            quarantineName.clear();
        }
        if (quarantine < 0)
        {
            error = "Unable to create private audio retraction quarantine";
            return false;
        }
        const auto cleanupQuarantine = [&]() noexcept {
            std::string ignored;
            (void) quarantineAndRemoveHeldEntry(
                quarantine,
                impl_->parent,
                quarantineName,
                true,
                ignored);
            ::close(quarantine);
            quarantine = -1;
        };
        bool moved = false;
#if defined(__APPLE__) || defined(__linux__)
        moved = renameWithoutReplacing(
            impl_->parent,
            impl_->destinationName.c_str(),
            quarantine,
            impl_->destinationName.c_str());
#else
        moved = ::renameat(
            impl_->parent,
            impl_->destinationName.c_str(),
            quarantine,
            impl_->destinationName.c_str()) == 0;
#endif
        if (!moved)
        {
            error = "Unable to quarantine published audio before retraction: "
                + std::error_code(errno, std::generic_category()).message();
            cleanupQuarantine();
            return false;
        }
        bool exact = sameRegularFileIdentity(
            impl_->temporary,
            quarantine,
            impl_->destinationName);
        if (exact && impl_->destinationBytesVerified)
        {
            std::vector<std::uint8_t> currentBytes;
            std::string verificationError;
            exact = readHeldPublicationFile(
                    impl_->temporary,
                    impl_->verifiedDestinationBytes.size(),
                    currentBytes,
                    verificationError)
                && currentBytes == impl_->verifiedDestinationBytes;
        }
        if (!exact)
        {
            bool restored = false;
#if defined(__APPLE__) || defined(__linux__)
            restored = renameWithoutReplacing(
                quarantine,
                impl_->destinationName.c_str(),
                impl_->parent,
                impl_->destinationName.c_str());
#else
            restored = ::renameat(
                quarantine,
                impl_->destinationName.c_str(),
                impl_->parent,
                impl_->destinationName.c_str()) == 0;
#endif
            cleanupQuarantine();
            error = restored
                ? "Published audio identity or bytes changed before retraction"
                : "Published audio changed and fail-closed restoration failed";
            return false;
        }
        std::string removalError;
        if (!quarantineAndRemoveHeldEntry(
                impl_->temporary,
                quarantine,
                impl_->destinationName,
                false,
                removalError))
        {
            bool restored = false;
#if defined(__APPLE__) || defined(__linux__)
            restored = renameWithoutReplacing(
                quarantine,
                impl_->destinationName.c_str(),
                impl_->parent,
                impl_->destinationName.c_str());
#else
            restored = ::renameat(
                quarantine,
                impl_->destinationName.c_str(),
                impl_->parent,
                impl_->destinationName.c_str()) == 0;
#endif
            error = restored
                ? "Unable to remove quarantined exact published audio file: "
                    + removalError
                : "Unable to remove or restore quarantined exact published audio file";
            cleanupQuarantine();
            return false;
        }
        cleanupQuarantine();
        ::close(impl_->temporary);
        impl_->temporary = -1;
#endif
        published_ = false;
        impl_->publicationInstalled = false;
        return true;
    }
    catch (...)
    {
        try
        {
            error = "Unable to retract exact published audio file";
        }
        catch (...)
        {
        }
        return false;
    }
}

bool AudioFilePublication::discard(std::string& error) noexcept
{
    try
    {
        error.clear();
        if (published_ || impl_ == nullptr)
            return true;
        bool ok = true;
#if defined(_WIN32)
        if (impl_->temporary == INVALID_HANDLE_VALUE)
        {
            impl_->temporary = ::CreateFileW(
                temporaryPath_.c_str(),
                DELETE | FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
        }
        if (impl_->temporary != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!::SetFileInformationByHandle(
                    impl_->temporary,
                    FileDispositionInfo,
                    &disposition,
                    sizeof(disposition)))
            {
                error = "Unable to remove private audio staging file";
                ok = false;
            }
            ::CloseHandle(impl_->temporary);
            impl_->temporary = INVALID_HANDLE_VALUE;
        }
        if (impl_->staging != INVALID_HANDLE_VALUE)
        {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!::SetFileInformationByHandle(
                    impl_->staging,
                    FileDispositionInfo,
                    &disposition,
                    sizeof(disposition)))
            {
                if (!error.empty())
                    error += "; ";
                error += "Unable to remove private audio staging directory";
                ok = false;
            }
            ::CloseHandle(impl_->staging);
            impl_->staging = INVALID_HANDLE_VALUE;
        }
#else
        if (impl_->temporary < 0 && impl_->staging >= 0)
        {
            impl_->temporary = ::openat(
                impl_->staging,
                impl_->temporaryName.c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (impl_->staging >= 0)
        {
            if (impl_->temporary >= 0)
            {
                std::string cleanupError;
                if (!quarantineAndRemoveHeldEntry(
                        impl_->temporary,
                        impl_->staging,
                        impl_->temporaryName,
                        false,
                        cleanupError))
                {
                    error = "Unable to remove private audio staging file: "
                        + cleanupError;
                    ok = false;
                }
            }
            else
            {
                struct stat status{};
                if (::fstatat(
                        impl_->staging,
                        impl_->temporaryName.c_str(),
                        &status,
                        AT_SYMLINK_NOFOLLOW) == 0)
                {
                    error = "Private audio staging leaf no longer names the held file";
                    ok = false;
                }
            }
            if (impl_->temporary >= 0)
            {
                ::close(impl_->temporary);
                impl_->temporary = -1;
            }
            if (ok)
            {
                std::string cleanupError;
                if (!quarantineAndRemoveHeldEntry(
                        impl_->staging,
                        impl_->parent,
                        impl_->stagingName,
                        true,
                        cleanupError))
                {
                    if (!error.empty())
                        error += "; ";
                    error += "Unable to remove private audio staging directory: "
                        + cleanupError;
                    ok = false;
                }
            }
            ::close(impl_->staging);
            impl_->staging = -1;
        }
#endif
        return ok;
    }
    catch (...)
    {
        try
        {
            error = "Audio staging cleanup failed";
        }
        catch (...)
        {
        }
        return false;
    }
}
}
