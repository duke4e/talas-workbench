#include "runtime/component_probe/macos/ComponentBundleProbeMaterialization.h"

#include <CommonCrypto/CommonDigest.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <span>
#include <string_view>

namespace talas::runtime::component_probe
{
namespace
{
constexpr char materializationPrefix[] = "TalasComponentProbe.v1.";
// Matches this version and the mkdtemp-named materializations before it.
constexpr std::string_view materializationFamilyPrefix = "TalasComponentProbe.";
constexpr char materializedEntryName[] = "entry.bundle";
constexpr std::array<char, 16> hexadecimalDigits{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
};

[[nodiscard]] bool sameIdentity(
    const struct stat& left,
    const struct stat& right) noexcept
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino
        && left.st_uid == right.st_uid
        && (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

[[nodiscard]] bool digestFile(
    int descriptor,
    std::array<std::uint8_t, componentTrustDigestBytes>& digest) noexcept
{
    CC_SHA256_CTX context{};
    if (CC_SHA256_Init(&context) != 1)
        return false;
    std::array<std::byte, 16U * 1024U> buffer{};
    std::uint64_t offset = 0;
    while (true)
    {
        const auto amount = pread(
            descriptor,
            buffer.data(),
            buffer.size(),
            static_cast<off_t>(offset));
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0)
            return false;
        if (amount == 0)
            break;
        if (amount > std::numeric_limits<CC_LONG>::max()
            || CC_SHA256_Update(
                   &context,
                   buffer.data(),
                   static_cast<CC_LONG>(amount)) != 1)
        {
            return false;
        }
        offset += static_cast<std::uint64_t>(amount);
    }
    return CC_SHA256_Final(digest.data(), &context) == 1;
}

[[nodiscard]] bool digestBytes(
    std::span<const std::byte> bytes,
    std::array<std::uint8_t, componentTrustDigestBytes>& digest) noexcept
{
    return !bytes.empty() && bytes.size() <= std::numeric_limits<CC_LONG>::max()
        && CC_SHA256(
               bytes.data(),
               static_cast<CC_LONG>(bytes.size()),
               digest.data()) != nullptr;
}

[[nodiscard]] bool matchesExpectedPrefix(
    int descriptor,
    std::span<const std::byte> expectedBytes,
    std::size_t actualBytes) noexcept
{
    std::array<std::byte, 16U * 1024U> buffer{};
    std::size_t offset = 0;
    while (offset < actualBytes)
    {
        const auto amountToRead = std::min(
            buffer.size(),
            actualBytes - offset);
        const auto amount = pread(
            descriptor,
            buffer.data(),
            amountToRead,
            static_cast<off_t>(offset));
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount <= 0
            || static_cast<std::size_t>(amount) > amountToRead
            || !std::equal(
                buffer.begin(),
                buffer.begin() + amount,
                expectedBytes.begin() + offset))
        {
            return false;
        }
        offset += static_cast<std::size_t>(amount);
    }
    return true;
}

[[nodiscard]] bool removeExactDirectory(
    const char* path,
    const struct stat& openedDirectory) noexcept
{
    struct stat pathStatus{};
    return lstat(path, &pathStatus) == 0
        && sameIdentity(openedDirectory, pathStatus)
        && S_ISDIR(pathStatus.st_mode) && pathStatus.st_uid == getuid()
        && (pathStatus.st_mode & 0777) == 0700 && rmdir(path) == 0;
}
}

bool componentBundleProbeMaterializationPathV1(
    const ComponentBundleProbeRequestV1& request,
    std::array<char, PATH_MAX>& path) noexcept
{
    path.fill(0);
    if (validateComponentBundleProbeRequestV1(request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    std::array<char, PATH_MAX> temporaryRoot{};
    const auto rootBytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        temporaryRoot.data(),
        temporaryRoot.size());
    if (rootBytes == 0 || rootBytes > temporaryRoot.size())
        return false;
    const auto rootLength = std::strlen(temporaryRoot.data());
    const auto separatorBytes = rootLength != 0
            && temporaryRoot[rootLength - 1U] == '/'
        ? 0U
        : 1U;
    constexpr std::size_t generationHexBytes = 16;
    constexpr std::size_t nonceHexBytes = componentProbeNonceBytes * 2U;
    const auto required = rootLength + separatorBytes
        + (sizeof(materializationPrefix) - 1U) + generationHexBytes + 1U
        + nonceHexBytes + 1U;
    if (rootLength == 0 || required > path.size())
        return false;

    auto position = std::size_t{0};
    std::memcpy(path.data(), temporaryRoot.data(), rootLength);
    position += rootLength;
    if (separatorBytes != 0)
        path[position++] = '/';
    std::memcpy(
        path.data() + position,
        materializationPrefix,
        sizeof(materializationPrefix) - 1U);
    position += sizeof(materializationPrefix) - 1U;
    for (std::size_t index = 0; index < generationHexBytes; ++index)
    {
        const auto shift = (generationHexBytes - 1U - index) * 4U;
        path[position++] = hexadecimalDigits[
            (request.artifactGeneration >> shift) & 0xfU];
    }
    path[position++] = '.';
    for (const auto byte : request.validationNonce)
    {
        path[position++] = hexadecimalDigits[(byte >> 4U) & 0xfU];
        path[position++] = hexadecimalDigits[byte & 0xfU];
    }
    path[position] = '\0';
    return true;
}

std::size_t sweepStaleComponentBundleProbeMaterializationsV1(
    std::uint64_t minimumAgeSeconds) noexcept
{
    std::array<char, PATH_MAX> temporaryRoot{};
    const auto rootBytes = confstr(
        _CS_DARWIN_USER_TEMP_DIR,
        temporaryRoot.data(),
        temporaryRoot.size());
    if (rootBytes == 0 || rootBytes > temporaryRoot.size())
        return 0;
    const auto rootDescriptor = open(
        temporaryRoot.data(),
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rootDescriptor < 0)
        return 0;
    const auto listingDescriptor = dup(rootDescriptor);
    auto* listing = listingDescriptor < 0 ? nullptr : fdopendir(listingDescriptor);
    if (listing == nullptr)
    {
        if (listingDescriptor >= 0)
            static_cast<void>(close(listingDescriptor));
        static_cast<void>(close(rootDescriptor));
        return 0;
    }
    const auto now = std::time(nullptr);
    std::size_t removed = 0;
    while (const auto* entry = readdir(listing))
    {
        const std::string_view name(entry->d_name);
        struct stat directoryStatus{};
        if (!name.starts_with(materializationFamilyPrefix)
            || fstatat(rootDescriptor, entry->d_name, &directoryStatus,
                   AT_SYMLINK_NOFOLLOW) != 0
            || !S_ISDIR(directoryStatus.st_mode)
            || directoryStatus.st_uid != getuid()
            || (directoryStatus.st_mode & 0777) != 0700
            || now < directoryStatus.st_mtime
            || static_cast<std::uint64_t>(now - directoryStatus.st_mtime)
                < minimumAgeSeconds)
        {
            continue;
        }
        const auto directoryDescriptor = openat(
            rootDescriptor,
            entry->d_name,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directoryDescriptor < 0)
            continue;
        struct stat openedStatus{};
        struct stat entryStatus{};
        const auto sameDirectory = fstat(directoryDescriptor, &openedStatus) == 0
            && openedStatus.st_dev == directoryStatus.st_dev
            && openedStatus.st_ino == directoryStatus.st_ino;
        const auto entryState = fstatat(directoryDescriptor, materializedEntryName,
            &entryStatus, AT_SYMLINK_NOFOLLOW);
        const auto entryMissing = entryState != 0 && errno == ENOENT;
        const auto entryRemovable = entryState == 0
            && S_ISREG(entryStatus.st_mode) && entryStatus.st_uid == getuid()
            && entryStatus.st_nlink == 1;
        if (sameDirectory && (entryMissing
                || (entryRemovable
                    && unlinkat(directoryDescriptor, materializedEntryName, 0) == 0))
            && unlinkat(rootDescriptor, entry->d_name, AT_REMOVEDIR) == 0)
        {
            ++removed;
        }
        static_cast<void>(close(directoryDescriptor));
    }
    static_cast<void>(closedir(listing));
    static_cast<void>(close(rootDescriptor));
    return removed;
}

bool cleanupComponentBundleProbeMaterializationV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> expectedEntryBytes) noexcept
{
    if (validateComponentBundleProbeExpectedAbiV1(expectedAbi, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid
        || expectedEntryBytes.size() != expectedAbi.entryByteCount)
    {
        return false;
    }
    std::array<std::uint8_t, componentTrustDigestBytes> expectedDigest{};
    if (expectedEntryBytes.empty()
        || !digestBytes(expectedEntryBytes, expectedDigest)
        || expectedDigest != expectedAbi.entryDigest)
    {
        return false;
    }
    std::array<char, PATH_MAX> directoryPath{};
    if (!componentBundleProbeMaterializationPathV1(request, directoryPath))
        return false;
    const auto directoryDescriptor = open(
        directoryPath.data(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directoryDescriptor < 0)
        return errno == ENOENT;

    struct stat directoryStatus{};
    if (fstat(directoryDescriptor, &directoryStatus) != 0
        || !S_ISDIR(directoryStatus.st_mode)
        || directoryStatus.st_uid != getuid()
        || (directoryStatus.st_mode & 0777) != 0700)
    {
        static_cast<void>(close(directoryDescriptor));
        return false;
    }

    const auto fileDescriptor = openat(
        directoryDescriptor,
        materializedEntryName,
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fileDescriptor < 0)
    {
        const auto missing = errno == ENOENT;
        const auto removed = missing
            && removeExactDirectory(
                directoryPath.data(),
                directoryStatus);
        static_cast<void>(close(directoryDescriptor));
        return removed;
    }

    struct stat beforeDigest{};
    std::array<std::uint8_t, componentTrustDigestBytes> actualDigest{};
    struct stat afterDigest{};
    const auto safeFile = fstat(fileDescriptor, &beforeDigest) == 0
        && S_ISREG(beforeDigest.st_mode)
        && beforeDigest.st_uid == getuid() && beforeDigest.st_nlink == 1
        && beforeDigest.st_size >= 0
        && static_cast<std::uint64_t>(beforeDigest.st_size)
            <= expectedAbi.entryByteCount
        && ((beforeDigest.st_mode & 0777) == 0600
            || (beforeDigest.st_mode & 0777) == 0500)
        && matchesExpectedPrefix(
            fileDescriptor,
            expectedEntryBytes,
            static_cast<std::size_t>(beforeDigest.st_size))
        && (static_cast<std::uint64_t>(beforeDigest.st_size)
                    != expectedAbi.entryByteCount
            || (digestFile(fileDescriptor, actualDigest)
                && actualDigest == expectedAbi.entryDigest))
        && fstat(fileDescriptor, &afterDigest) == 0
        && sameIdentity(beforeDigest, afterDigest)
        && beforeDigest.st_nlink == afterDigest.st_nlink
        && beforeDigest.st_size == afterDigest.st_size;
    if (!safeFile)
    {
        static_cast<void>(close(fileDescriptor));
        static_cast<void>(close(directoryDescriptor));
        return false;
    }
    const auto unlinked = unlinkat(
        directoryDescriptor,
        materializedEntryName,
        0) == 0;
    const auto fileClosed = close(fileDescriptor) == 0;
    const auto removed = unlinked && fileClosed
        && removeExactDirectory(directoryPath.data(), directoryStatus);
    const auto directoryClosed = close(directoryDescriptor) == 0;
    return removed && directoryClosed;
}
}
