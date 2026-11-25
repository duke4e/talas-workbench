#include "runtime/component_bundle/ComponentBundleInspection.h"

#include "talas/project/ProjectAssetInspection.h"

#include <algorithm>
#include <map>
#include <set>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace talas::runtime::component_bundle
{
#if defined(__unix__) || defined(__APPLE__)
namespace
{
class UniqueFd
{
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

private:
    int fd_ = -1;
};

bool fail(
    ComponentBundleInspectionResult& result,
    ComponentBundleInspectionErrorCode code,
    std::string path,
    std::string summary)
{
    const auto safePath = path.size() <= maximumComponentBundlePathBytes
        && std::ranges::all_of(
            path,
            [](unsigned char character) {
                return character >= 0x20U && character <= 0x7eU;
            });
    if (!safePath)
        path = "$";
    if (result.errors.empty())
    {
        result.errors.push_back({
            code,
            std::move(path),
            std::move(summary),
        });
    }
    result.snapshot.reset();
    return false;
}

[[nodiscard]] bool cancelled(
    const ComponentBundleInspectionCancellationCheck& shouldCancel)
{
    return shouldCancel && shouldCancel();
}

[[nodiscard]] bool sameMetadata(const struct stat& first, const struct stat& second)
{
    if (first.st_dev != second.st_dev
        || first.st_ino != second.st_ino
        || first.st_mode != second.st_mode
        || first.st_nlink != second.st_nlink
        || first.st_size != second.st_size)
    {
        return false;
    }
#if defined(__APPLE__)
    return first.st_mtimespec.tv_sec == second.st_mtimespec.tv_sec
        && first.st_mtimespec.tv_nsec == second.st_mtimespec.tv_nsec
        && first.st_ctimespec.tv_sec == second.st_ctimespec.tv_sec
        && first.st_ctimespec.tv_nsec == second.st_ctimespec.tv_nsec;
#else
    return first.st_mtim.tv_sec == second.st_mtim.tv_sec
        && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec
        && first.st_ctim.tv_sec == second.st_ctim.tv_sec
        && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
#endif
}

[[nodiscard]] std::vector<std::string_view> pathSegments(std::string_view path)
{
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start < path.size())
    {
        const auto separator = path.find('/', start);
        segments.push_back(path.substr(
            start,
            separator == std::string_view::npos
                ? path.size() - start
                : separator - start));
        if (separator == std::string_view::npos)
            break;
        start = separator + 1U;
    }
    return segments;
}

[[nodiscard]] bool openDirectorySegment(
    int parent,
    std::string_view segment,
    std::string_view diagnosticPath,
    UniqueFd& output,
    ComponentBundleInspectionResult& result)
{
    const std::string name(segment);
    struct stat status{};
    if (::fstatat(parent, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(diagnosticPath),
            "Component bundle directory entry is unavailable");
    }
    if (S_ISLNK(status.st_mode))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::SymlinkDenied,
            std::string(diagnosticPath),
            "Component bundle symlinks are not allowed");
    }
    if (!S_ISDIR(status.st_mode))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::NonRegularEntry,
            std::string(diagnosticPath),
            "Component bundle path segment is not a directory");
    }
    output = UniqueFd(::openat(
        parent,
        name.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!output.valid())
    {
        return fail(
            result,
            errno == ELOOP
                ? ComponentBundleInspectionErrorCode::SymlinkDenied
                : ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(diagnosticPath),
            "Component bundle directory cannot be opened safely");
    }
    return true;
}

[[nodiscard]] bool readRegularFile(
    int rootFd,
    std::string_view path,
    std::optional<std::uint64_t> expectedBytes,
    std::uint64_t maximumBytes,
    const ComponentBundleInspectionCancellationCheck& shouldCancel,
    std::vector<std::uint8_t>& bytes,
    ComponentBundleInspectionResult& result)
{
    const auto segments = pathSegments(path);
    if (segments.empty())
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(path),
            "Component bundle file path is empty");
    }

    int parent = rootFd;
    std::vector<UniqueFd> directories;
    std::string traversed;
    for (std::size_t index = 0; index + 1U < segments.size(); ++index)
    {
        if (!traversed.empty())
            traversed.push_back('/');
        traversed += segments[index];
        UniqueFd directory;
        if (!openDirectorySegment(
                parent,
                segments[index],
                traversed,
                directory,
                result))
        {
            return false;
        }
        parent = directory.get();
        directories.push_back(std::move(directory));
    }

    const std::string fileName(segments.back());
    struct stat linkStatus{};
    if (::fstatat(
            parent,
            fileName.c_str(),
            &linkStatus,
            AT_SYMLINK_NOFOLLOW) != 0)
    {
        return fail(
            result,
            path == "manifest.json"
                ? ComponentBundleInspectionErrorCode::ManifestUnavailable
                : ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(path),
            "Component bundle file is unavailable");
    }
    if (S_ISLNK(linkStatus.st_mode))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::SymlinkDenied,
            std::string(path),
            "Component bundle symlinks are not allowed");
    }
    if (!S_ISREG(linkStatus.st_mode))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::NonRegularEntry,
            std::string(path),
            "Component bundle payload is not a regular file");
    }

    // O_NONBLOCK keeps a FIFO swapped in after fstatat from blocking open.
    UniqueFd file(::openat(
        parent,
        fileName.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (!file.valid())
    {
        return fail(
            result,
            errno == ELOOP
                ? ComponentBundleInspectionErrorCode::SymlinkDenied
                : ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(path),
            "Component bundle file cannot be opened safely");
    }
    struct stat before{};
    if (::fstat(file.get(), &before) != 0 || !S_ISREG(before.st_mode))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::NonRegularEntry,
            std::string(path),
            "Component bundle payload is not a regular file");
    }
    if (before.st_nlink != 1)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::HardlinkDenied,
            std::string(path),
            "Component bundle hardlinked files are not allowed");
    }
    if (before.st_size < 0
        || static_cast<std::uint64_t>(before.st_size) > maximumBytes)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::BudgetExceeded,
            std::string(path),
            "Component bundle file exceeds its byte limit");
    }
    if (expectedBytes
        && static_cast<std::uint64_t>(before.st_size) != *expectedBytes)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::SizeMismatch,
            std::string(path),
            "Component bundle payload size does not match the manifest");
    }
    if (cancelled(shouldCancel))
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::Cancelled,
            std::string(path),
            "Component bundle inspection was cancelled");
    }

    bytes.resize(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (cancelled(shouldCancel))
        {
            bytes.clear();
            return fail(
                result,
                ComponentBundleInspectionErrorCode::Cancelled,
                std::string(path),
                "Component bundle inspection was cancelled");
        }
        const auto count = ::read(
            file.get(),
            bytes.data() + offset,
            std::min<std::size_t>(64U * 1024U, bytes.size() - offset));
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
        {
            bytes.clear();
            return fail(
                result,
                ComponentBundleInspectionErrorCode::ConcurrentMutation,
                std::string(path),
                "Component bundle file changed during inspection");
        }
        offset += static_cast<std::size_t>(count);
    }

    struct stat after{};
    if (::fstat(file.get(), &after) != 0 || !sameMetadata(before, after))
    {
        bytes.clear();
        return fail(
            result,
            ComponentBundleInspectionErrorCode::ConcurrentMutation,
            std::string(path),
            "Component bundle file changed during inspection");
    }
    return true;
}

struct DirectoryEntryIdentity
{
    dev_t device = 0;
    ino_t inode = 0;
    mode_t mode = 0;
    nlink_t linkCount = 0;
    off_t byteCount = 0;
    time_t modifiedSeconds = 0;
    long modifiedNanoseconds = 0;
    time_t changedSeconds = 0;
    long changedNanoseconds = 0;

    [[nodiscard]] bool operator==(
        const DirectoryEntryIdentity&) const noexcept = default;
};

[[nodiscard]] DirectoryEntryIdentity directoryEntryIdentity(
    const struct stat& status) noexcept
{
    DirectoryEntryIdentity identity;
    identity.device = status.st_dev;
    identity.inode = status.st_ino;
    identity.mode = status.st_mode;
    identity.linkCount = status.st_nlink;
    identity.byteCount = status.st_size;
#if defined(__APPLE__)
    identity.modifiedSeconds = status.st_mtimespec.tv_sec;
    identity.modifiedNanoseconds = status.st_mtimespec.tv_nsec;
    identity.changedSeconds = status.st_ctimespec.tv_sec;
    identity.changedNanoseconds = status.st_ctimespec.tv_nsec;
#else
    identity.modifiedSeconds = status.st_mtim.tv_sec;
    identity.modifiedNanoseconds = status.st_mtim.tv_nsec;
    identity.changedSeconds = status.st_ctim.tv_sec;
    identity.changedNanoseconds = status.st_ctim.tv_nsec;
#endif
    return identity;
}

struct DirectorySnapshot
{
    std::set<std::string> files;
    std::set<std::string> directories;
    std::map<std::string, DirectoryEntryIdentity> identities;
    std::size_t entryCount = 0;
};

[[nodiscard]] bool scanDirectory(
    int directoryFd,
    std::string_view prefix,
    std::size_t depth,
    DirectorySnapshot& snapshot,
    ComponentBundleInspectionResult& result)
{
    if (depth > maximumComponentBundleDirectoryDepth)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::BudgetExceeded,
            std::string(prefix),
            "Component bundle directory depth exceeds the limit");
    }
    const auto scanFd = ::openat(
        directoryFd,
        ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scanFd < 0)
    {
        return fail(
            result,
            ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(prefix),
            "Component bundle directory cannot be inspected");
    }
    DIR* stream = ::fdopendir(scanFd);
    if (stream == nullptr)
    {
        ::close(scanFd);
        return fail(
            result,
            ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(prefix),
            "Component bundle directory cannot be inspected");
    }

    bool ok = true;
    errno = 0;
    while (const auto* entry = ::readdir(stream))
    {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        if (++snapshot.entryCount > maximumComponentBundleDirectoryEntries)
        {
            ok = fail(
                result,
                ComponentBundleInspectionErrorCode::BudgetExceeded,
                std::string(prefix),
                "Component bundle directory entries exceed the limit");
            break;
        }
        std::string path(prefix);
        if (!path.empty())
            path.push_back('/');
        path += name;
        if (path.size() > maximumComponentBundlePathBytes)
        {
            ok = fail(
                result,
                ComponentBundleInspectionErrorCode::BudgetExceeded,
                "$",
                "Component bundle entry path exceeds the limit");
            break;
        }
        struct stat status{};
        const std::string entryName(name);
        if (::fstatat(
                directoryFd,
                entryName.c_str(),
                &status,
                AT_SYMLINK_NOFOLLOW) != 0)
        {
            ok = fail(
                result,
                ComponentBundleInspectionErrorCode::ConcurrentMutation,
                path,
                "Component bundle directory changed during inspection");
            break;
        }
        if (S_ISLNK(status.st_mode))
        {
            ok = fail(
                result,
                ComponentBundleInspectionErrorCode::SymlinkDenied,
                path,
                "Component bundle symlinks are not allowed");
            break;
        }
        snapshot.identities.emplace(path, directoryEntryIdentity(status));
        if (S_ISDIR(status.st_mode))
        {
            snapshot.directories.insert(path);
            UniqueFd child(::openat(
                directoryFd,
                entryName.c_str(),
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
            if (!child.valid()
                || !scanDirectory(child.get(), path, depth + 1U, snapshot, result))
            {
                ok = false;
                break;
            }
        }
        else if (S_ISREG(status.st_mode))
        {
            if (status.st_nlink != 1)
            {
                ok = fail(
                    result,
                    ComponentBundleInspectionErrorCode::HardlinkDenied,
                    path,
                    "Component bundle hardlinked files are not allowed");
                break;
            }
            snapshot.files.insert(path);
        }
        else
        {
            ok = fail(
                result,
                ComponentBundleInspectionErrorCode::NonRegularEntry,
                path,
                "Component bundle contains a non-regular entry");
            break;
        }
    }
    if (ok && errno != 0)
    {
        ok = fail(
            result,
            ComponentBundleInspectionErrorCode::ReadFailed,
            std::string(prefix),
            "Component bundle directory scan failed");
    }
    ::closedir(stream);
    return ok;
}

void addExpectedPath(
    std::string_view path,
    std::set<std::string>& files,
    std::set<std::string>& directories)
{
    files.insert(std::string(path));
    std::size_t separator = path.find('/');
    while (separator != std::string_view::npos)
    {
        directories.insert(std::string(path.substr(0, separator)));
        separator = path.find('/', separator + 1U);
    }
}

[[nodiscard]] bool rejectUnexpectedEntries(
    const DirectorySnapshot& actual,
    const std::set<std::string>& expectedFiles,
    const std::set<std::string>& expectedDirectories,
    ComponentBundleInspectionResult& result)
{
    for (const auto& file : actual.files)
    {
        if (!expectedFiles.contains(file))
        {
            return fail(
                result,
                ComponentBundleInspectionErrorCode::UndeclaredEntry,
                file,
                "Component bundle contains an undeclared file");
        }
    }
    for (const auto& directory : actual.directories)
    {
        if (!expectedDirectories.contains(directory))
        {
            return fail(
                result,
                ComponentBundleInspectionErrorCode::UndeclaredEntry,
                directory,
                "Component bundle contains an undeclared directory");
        }
    }
    return true;
}

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
decodeDigest(std::string_view value)
{
    std::array<std::uint8_t, componentTrustDigestBytes> digest{};
    if (value.size() != digest.size() * 2U)
        return {};
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index)
    {
        const auto high = nibble(value[index * 2U]);
        const auto low = nibble(value[index * 2U + 1U]);
        if (high < 0 || low < 0)
            return {};
        digest[index] = static_cast<std::uint8_t>(
            (high << 4U) | low);
    }
    return digest;
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
contentDigestForSnapshot(const ComponentBundleSnapshot& snapshot)
{
    constexpr std::string_view domain = "talas.component-bundle.content.v1";
    std::vector<std::uint8_t> transcript;
    transcript.reserve(
        snapshot.canonicalManifest.size()
        + snapshot.payloads.size() * 300U + 64U);
    transcript.insert(transcript.end(), domain.begin(), domain.end());
    transcript.push_back(0);
    appendU64(transcript, snapshot.canonicalManifest.size());
    transcript.insert(
        transcript.end(),
        snapshot.canonicalManifest.begin(),
        snapshot.canonicalManifest.end());
    appendU32(
        transcript,
        static_cast<std::uint32_t>(snapshot.payloads.size()));
    for (const auto& payload : snapshot.payloads)
    {
        appendU16(transcript, static_cast<std::uint16_t>(payload.path.size()));
        transcript.insert(
            transcript.end(),
            payload.path.begin(),
            payload.path.end());
        appendU64(transcript, payload.bytes.size());
        transcript.insert(
            transcript.end(),
            payload.sha256.begin(),
            payload.sha256.end());
    }
    return decodeDigest(talas::project::calculateProjectAssetSha256(transcript));
}
}

std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentBundleSnapshotContentDigest(
    const ComponentBundleSnapshot& snapshot)
{
    return contentDigestForSnapshot(snapshot);
}

bool validateComponentBundleSnapshotIdentity(
    const ComponentBundleSnapshot& snapshot)
{
    const auto canonical =
        writeCanonicalComponentBundleManifest(snapshot.manifest);
    if (!canonical.ok()
        || canonical.canonicalJson != snapshot.canonicalManifest
        || snapshot.payloads.size() != snapshot.manifest.files.size() + 1U
        || !std::ranges::is_sorted(
            snapshot.payloads,
            {},
            &ComponentBundleFrozenPayload::path))
    {
        return false;
    }

    struct ExpectedPayload
    {
        ComponentBundlePayloadRecord record;
        ComponentBundleFilePurpose purpose =
            ComponentBundleFilePurpose::Resource;
        bool isEntry = false;
    };
    std::vector<ExpectedPayload> expected;
    expected.reserve(snapshot.manifest.files.size() + 1U);
    expected.push_back({
        snapshot.manifest.entry,
        ComponentBundleFilePurpose::Resource,
        true});
    for (const auto& file : snapshot.manifest.files)
        expected.push_back({file, file.purpose, false});
    std::ranges::sort(
        expected,
        {},
        [](const ExpectedPayload& value) -> const std::string& {
            return value.record.path;
        });

    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto& frozen = snapshot.payloads[index];
        const auto& declared = expected[index];
        if (frozen.path != declared.record.path
            || frozen.purpose != declared.purpose
            || frozen.isEntry != declared.isEntry
            || frozen.bytes.size() != declared.record.byteCount
            || frozen.sha256 != declared.record.sha256
            || decodeDigest(
                   talas::project::calculateProjectAssetSha256(frozen.bytes))
                != declared.record.sha256)
        {
            return false;
        }
    }

    return calculateComponentBundleSnapshotContentDigest(snapshot)
        == snapshot.contentDigest;
}

ComponentBundleInspectionResult inspectComponentBundle(
    const std::filesystem::path& root,
    ComponentBundleInspectionCancellationCheck shouldCancel)
{
    ComponentBundleInspectionResult result;
    if (cancelled(shouldCancel))
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::Cancelled,
            "$",
            "Component bundle inspection was cancelled");
        return result;
    }

    struct stat rootLinkStatus{};
    if (::lstat(root.c_str(), &rootLinkStatus) != 0)
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::InvalidRoot,
            "$",
            "Component bundle root is unavailable");
        return result;
    }
    if (S_ISLNK(rootLinkStatus.st_mode))
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::SymlinkDenied,
            "$",
            "Component bundle root cannot be a symlink");
        return result;
    }
    UniqueFd rootFd(::open(
        root.c_str(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!rootFd.valid())
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::InvalidRoot,
            "$",
            "Component bundle root is not a readable directory");
        return result;
    }

    std::vector<std::uint8_t> manifestBytes;
    if (!readRegularFile(
            rootFd.get(),
            "manifest.json",
            std::nullopt,
            maximumComponentBundleManifestBytes,
            shouldCancel,
            manifestBytes,
            result))
    {
        return result;
    }
    const std::string manifestSource(
        reinterpret_cast<const char*>(manifestBytes.data()),
        manifestBytes.size());
    const auto parsed = parseComponentBundleManifest(manifestSource);
    if (!parsed.ok())
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::ManifestInvalid,
            "manifest.json",
            "Component bundle manifest is invalid");
        return result;
    }
    const auto canonical =
        writeCanonicalComponentBundleManifest(*parsed.manifest);
    if (!canonical.ok())
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::ManifestInvalid,
            "manifest.json",
            "Component bundle manifest cannot be canonicalized");
        return result;
    }

    std::set<std::string> expectedFiles;
    std::set<std::string> expectedDirectories;
    addExpectedPath("manifest.json", expectedFiles, expectedDirectories);
    addExpectedPath(
        parsed.manifest->entry.path,
        expectedFiles,
        expectedDirectories);
    for (const auto& file : parsed.manifest->files)
        addExpectedPath(file.path, expectedFiles, expectedDirectories);

    DirectorySnapshot beforeEntries;
    if (!scanDirectory(rootFd.get(), {}, 0, beforeEntries, result))
        return result;
    const auto hasDetachedSignature =
        beforeEntries.files.contains("signature.cms");
    if (hasDetachedSignature)
    {
        addExpectedPath(
            "signature.cms",
            expectedFiles,
            expectedDirectories);
    }
    if (!rejectUnexpectedEntries(
            beforeEntries,
            expectedFiles,
            expectedDirectories,
            result))
    {
        return result;
    }

    ComponentBundleSnapshot snapshot;
    snapshot.manifest = *parsed.manifest;
    snapshot.canonicalManifest = canonical.canonicalJson;
    snapshot.payloads.reserve(parsed.manifest->files.size() + 1U);

    const auto appendPayload = [&](
                                   const ComponentBundlePayloadRecord& record,
                                   ComponentBundleFilePurpose purpose,
                                   bool isEntry) {
        ComponentBundleFrozenPayload payload;
        payload.path = record.path;
        payload.purpose = purpose;
        payload.sha256 = record.sha256;
        payload.isEntry = isEntry;
        if (!readRegularFile(
                rootFd.get(),
                record.path,
                record.byteCount,
                maximumComponentBundleFileBytes,
                shouldCancel,
                payload.bytes,
                result))
        {
            return false;
        }
        const auto actualDigest = decodeDigest(
            talas::project::calculateProjectAssetSha256(payload.bytes));
        if (actualDigest != record.sha256)
        {
            return fail(
                result,
                ComponentBundleInspectionErrorCode::DigestMismatch,
                record.path,
                "Component bundle payload digest does not match the manifest");
        }
        snapshot.payloads.push_back(std::move(payload));
        return true;
    };

    if (!appendPayload(
            parsed.manifest->entry,
            ComponentBundleFilePurpose::Resource,
            true))
    {
        return result;
    }
    for (const auto& file : parsed.manifest->files)
    {
        if (!appendPayload(file, file.purpose, false))
            return result;
    }
    std::ranges::sort(
        snapshot.payloads,
        {},
        &ComponentBundleFrozenPayload::path);

    if (hasDetachedSignature)
    {
        std::vector<std::uint8_t> signatureBytes;
        if (!readRegularFile(
                rootFd.get(),
                "signature.cms",
                std::nullopt,
                maximumComponentBundleSignatureBytes,
                shouldCancel,
                signatureBytes,
                result))
        {
            return result;
        }
        snapshot.detachedSignatureCms = std::move(signatureBytes);
    }

    DirectorySnapshot afterEntries;
    if (!scanDirectory(rootFd.get(), {}, 0, afterEntries, result)
        || !rejectUnexpectedEntries(
            afterEntries,
            expectedFiles,
            expectedDirectories,
            result))
    {
        return result;
    }
    if (beforeEntries.files != afterEntries.files
        || beforeEntries.directories != afterEntries.directories
        || beforeEntries.identities != afterEntries.identities
        || afterEntries.files != expectedFiles
        || afterEntries.directories != expectedDirectories)
    {
        fail(
            result,
            ComponentBundleInspectionErrorCode::ConcurrentMutation,
            "$",
            "Component bundle directory changed during inspection");
        return result;
    }
    snapshot.contentDigest =
        calculateComponentBundleSnapshotContentDigest(snapshot);
    result.snapshot = std::move(snapshot);
    return result;
}
#else
ComponentBundleInspectionResult inspectComponentBundle(
    const std::filesystem::path& root,
    ComponentBundleInspectionCancellationCheck shouldCancel)
{
    (void)root;
    (void)shouldCancel;
    ComponentBundleInspectionResult result;
    result.errors.push_back({
        ComponentBundleInspectionErrorCode::InvalidRoot,
        "$",
        "Secure component bundle inspection is unavailable on this platform",
    });
    return result;
}
#endif
}
