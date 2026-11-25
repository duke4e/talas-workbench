#include "talas/project/ProjectAssetInspection.h"

#include "talas/core/SampleFileLayout.h"

#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace talas::project
{
namespace
{
struct BoundedBytesResult
{
    std::vector<std::uint8_t> bytes;
    std::optional<ProjectAssetInspectionError> error;
};

constexpr std::array<std::uint32_t, 64> sha256Constants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

struct Sha256
{
    std::array<std::uint32_t, 8> words = {
        0x6a09e667U,
        0xbb67ae85U,
        0x3c6ef372U,
        0xa54ff53aU,
        0x510e527fU,
        0x9b05688cU,
        0x1f83d9abU,
        0x5be0cd19U,
    };
    std::array<std::uint8_t, 64> buffer{};
    std::size_t buffered = 0;
    std::uint64_t totalBytes = 0;

    void transform(const std::uint8_t* block)
    {
        std::array<std::uint32_t, 64> schedule{};
        for (std::size_t index = 0; index < 16; ++index)
        {
            const auto offset = index * 4;
            schedule[index] =
                (static_cast<std::uint32_t>(block[offset]) << 24U)
                | (static_cast<std::uint32_t>(block[offset + 1]) << 16U)
                | (static_cast<std::uint32_t>(block[offset + 2]) << 8U)
                | static_cast<std::uint32_t>(block[offset + 3]);
        }
        for (std::size_t index = 16; index < schedule.size(); ++index)
        {
            const auto low = std::rotr(schedule[index - 15], 7)
                ^ std::rotr(schedule[index - 15], 18)
                ^ (schedule[index - 15] >> 3U);
            const auto high = std::rotr(schedule[index - 2], 17)
                ^ std::rotr(schedule[index - 2], 19)
                ^ (schedule[index - 2] >> 10U);
            schedule[index] = schedule[index - 16] + low
                + schedule[index - 7] + high;
        }

        auto a = words[0];
        auto b = words[1];
        auto c = words[2];
        auto d = words[3];
        auto e = words[4];
        auto f = words[5];
        auto g = words[6];
        auto h = words[7];
        for (std::size_t index = 0; index < schedule.size(); ++index)
        {
            const auto sigmaOne = std::rotr(e, 6)
                ^ std::rotr(e, 11) ^ std::rotr(e, 25);
            const auto choice = (e & f) ^ (~e & g);
            const auto temporaryOne = h + sigmaOne + choice
                + sha256Constants[index] + schedule[index];
            const auto sigmaZero = std::rotr(a, 2)
                ^ std::rotr(a, 13) ^ std::rotr(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto temporaryTwo = sigmaZero + majority;
            h = g;
            g = f;
            f = e;
            e = d + temporaryOne;
            d = c;
            c = b;
            b = a;
            a = temporaryOne + temporaryTwo;
        }
        words[0] += a;
        words[1] += b;
        words[2] += c;
        words[3] += d;
        words[4] += e;
        words[5] += f;
        words[6] += g;
        words[7] += h;
    }

    bool append(const std::uint8_t* bytes, std::size_t count)
    {
        if (count > std::numeric_limits<std::uint64_t>::max() - totalBytes)
            return false;
        totalBytes += static_cast<std::uint64_t>(count);
        while (count != 0)
        {
            const auto copied = std::min(count, buffer.size() - buffered);
            std::copy_n(bytes, copied, buffer.begin() + buffered);
            bytes += copied;
            count -= copied;
            buffered += copied;
            if (buffered == buffer.size())
            {
                transform(buffer.data());
                buffered = 0;
            }
        }
        return true;
    }

    [[nodiscard]] std::string finish()
    {
        const auto bitCount = totalBytes * 8U;
        buffer[buffered++] = 0x80U;
        if (buffered > 56)
        {
            std::fill(buffer.begin() + buffered, buffer.end(), 0U);
            transform(buffer.data());
            buffered = 0;
        }
        std::fill(buffer.begin() + buffered, buffer.begin() + 56, 0U);
        for (std::size_t index = 0; index < 8; ++index)
        {
            buffer[56 + index] = static_cast<std::uint8_t>(
                bitCount >> ((7U - index) * 8U));
        }
        transform(buffer.data());

        std::ostringstream output;
        output << std::hex << std::nouppercase << std::setfill('0');
        for (const auto word : words)
            output << std::setw(8) << word;
        return output.str();
    }
};

[[nodiscard]] ProjectAssetInspectionError makeError(
    ProjectAssetInspectionErrorCode code,
    std::string message,
    const std::filesystem::path& path)
{
    return {
        code,
        std::move(message),
        pathToUtf8(path),
    };
}

[[nodiscard]] BoundedBytesResult readDirectBoundedBytesImpl(
    const std::filesystem::path& path,
    std::size_t maximumBytes,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    BoundedBytesResult result;
    if (path.empty() || (shouldCancel && shouldCancel()))
    {
        result.error = makeError(
            path.empty()
                ? ProjectAssetInspectionErrorCode::Unavailable
                : ProjectAssetInspectionErrorCode::Cancelled,
            path.empty() ? "Asset path cannot be empty"
                         : "Asset inspection cancelled",
            path);
        return result;
    }
#if defined(_WIN32)
    std::error_code absoluteError;
    const auto absolutePath = std::filesystem::absolute(
        path, absoluteError).lexically_normal();
    if (absoluteError || absolutePath.empty() || !absolutePath.is_absolute())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to resolve direct asset authority",
            path);
        return result;
    }
    struct HandleChain
    {
        std::vector<HANDLE> values;
        ~HandleChain()
        {
            for (auto iterator = values.rbegin();
                 iterator != values.rend();
                 ++iterator)
            {
                if (*iterator != INVALID_HANDLE_VALUE)
                    ::CloseHandle(*iterator);
            }
        }
    } ancestors;
    const auto openDirectDirectory = [](
        const std::filesystem::path& directory) -> HANDLE {
        const auto handle = ::CreateFileW(
            directory.c_str(),
            FILE_READ_ATTRIBUTES,
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
    auto currentPath = absolutePath.root_path();
    auto current = openDirectDirectory(currentPath);
    if (current == INVALID_HANDLE_VALUE)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::NotRegularFile,
            "Asset volume root is indirect or unavailable",
            path);
        return result;
    }
    try
    {
        ancestors.values.push_back(current);
    }
    catch (...)
    {
        ::CloseHandle(current);
        throw;
    }
    for (const auto& component : absolutePath.parent_path().relative_path())
    {
        currentPath /= component;
        current = openDirectDirectory(currentPath);
        if (current == INVALID_HANDLE_VALUE)
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::NotRegularFile,
                "Asset path contains an indirect or unavailable directory",
                path);
            return result;
        }
        try
        {
            ancestors.values.push_back(current);
        }
        catch (...)
        {
            ::CloseHandle(current);
            throw;
        }
    }
    const auto file = ::CreateFileW(
        absolutePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to open direct asset file",
            path);
        return result;
    }
    struct HandleCloser
    {
        HANDLE value;
        ~HandleCloser() { ::CloseHandle(value); }
    } fileCloser{file};
    FILE_ATTRIBUTE_TAG_INFO fileAttributes{};
    LARGE_INTEGER size{};
    if (!::GetFileInformationByHandleEx(
            file, FileAttributeTagInfo, &fileAttributes, sizeof(fileAttributes))
        || (fileAttributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U
        || (fileAttributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U
        || !::GetFileSizeEx(file, &size) || size.QuadPart < 0
        || static_cast<std::uint64_t>(size.QuadPart) > maximumBytes)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::NotRegularFile,
            "Asset is indirect, invalid, or exceeds its byte bound",
            path);
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < result.bytes.size())
    {
        if (shouldCancel && shouldCancel())
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::Cancelled,
                "Asset inspection cancelled",
                path);
            return result;
        }
        DWORD read = 0;
        const auto requested = static_cast<DWORD>(std::min<std::size_t>(
            result.bytes.size() - offset,
            std::numeric_limits<DWORD>::max()));
        if (!::ReadFile(file, result.bytes.data() + offset, requested, &read, nullptr)
            || read == 0U)
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::ReadFailed,
                "Unable to read direct asset file",
                path);
            return result;
        }
        offset += read;
    }
    std::uint8_t extra = 0;
    DWORD extraRead = 0;
    LARGE_INTEGER finalSize{};
    if (!::ReadFile(file, &extra, 1U, &extraRead, nullptr)
        || extraRead != 0U
        || !::GetFileSizeEx(file, &finalSize)
        || finalSize.QuadPart != size.QuadPart)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset changed during direct inspection",
            path);
        return result;
    }
#else
    std::error_code absoluteError;
    const auto absolute = std::filesystem::absolute(path, absoluteError)
                              .lexically_normal();
    if (absoluteError || absolute.empty())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to resolve direct asset path",
            path);
        return result;
    }
    const auto relative = absolute.relative_path();
    auto component = relative.begin();
    const auto end = relative.end();
    if (component == end)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Direct asset path has no components",
            path);
        return result;
    }
    auto top = absolute.root_path() / *component;
    std::error_code topError;
    top = std::filesystem::weakly_canonical(top, topError);
    if (topError)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to resolve asset volume authority",
            path);
        return result;
    }
    int current = ::open(
        top.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to hold asset volume authority",
            path);
        return result;
    }
    struct DescriptorCloser
    {
        int value;
        ~DescriptorCloser() { if (value >= 0) ::close(value); }
    } closer{current};
    ++component;
    for (; component != end; ++component)
    {
        const auto next = std::next(component);
        const auto descriptor = ::openat(
            closer.value,
            component->c_str(),
            O_RDONLY | O_CLOEXEC | O_NOFOLLOW
                | (next == end ? 0 : O_DIRECTORY));
        if (descriptor < 0)
        {
            result.error = makeError(
                errno == ENOENT
                    ? ProjectAssetInspectionErrorCode::Unavailable
                    : ProjectAssetInspectionErrorCode::NotRegularFile,
                "Asset path is unavailable or contains an indirect component",
                path);
            return result;
        }
        ::close(closer.value);
        closer.value = descriptor;
    }
    struct stat before{};
    if (::fstat(closer.value, &before) != 0 || !S_ISREG(before.st_mode)
        || before.st_size < 0
        || static_cast<std::uint64_t>(before.st_size) > maximumBytes)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::NotRegularFile,
            "Asset is invalid or exceeds its byte bound",
            path);
        return result;
    }
    result.bytes.resize(static_cast<std::size_t>(before.st_size));
    std::size_t offset = 0;
    while (offset < result.bytes.size())
    {
        if (shouldCancel && shouldCancel())
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::Cancelled,
                "Asset inspection cancelled",
                path);
            return result;
        }
        const auto read = ::read(
            closer.value,
            result.bytes.data() + offset,
            result.bytes.size() - offset);
        if (read < 0 && errno == EINTR)
            continue;
        if (read <= 0)
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::ReadFailed,
                "Unable to read direct asset file",
                path);
            return result;
        }
        offset += static_cast<std::size_t>(read);
    }
    std::uint8_t extra = 0;
    ssize_t extraCount = 0;
    do
    {
        extraCount = ::read(closer.value, &extra, 1);
    } while (extraCount < 0 && errno == EINTR);
    struct stat after{};
    if (extraCount != 0 || ::fstat(closer.value, &after) != 0
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_size != after.st_size)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset changed during direct inspection",
            path);
        return result;
    }
#endif
    return result;
}

[[nodiscard]] BoundedBytesResult readDirectBoundedBytes(
    const std::filesystem::path& path,
    std::size_t maximumBytes,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    try
    {
        return readDirectBoundedBytesImpl(
            path, maximumBytes, shouldCancel);
    }
    catch (...)
    {
        BoundedBytesResult result;
        result.bytes.clear();
        try
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::ReadFailed,
                "Unable to allocate the bounded direct asset snapshot",
                path);
        }
        catch (...)
        {
        }
        return result;
    }
}

[[nodiscard]] BoundedBytesResult readBoundedPartialTrackBytes(
    const std::filesystem::path& path,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    BoundedBytesResult result;
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(
        path,
        statusError);
    if (statusError)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to inspect partial-track asset: "
                + statusError.message(),
            path);
        return result;
    }
    if (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::NotRegularFile,
            "Partial-track asset is not a direct regular file",
            path);
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ReadFailed,
            "Unable to open partial-track asset",
            path);
        return result;
    }
    try
    {
        std::array<std::uint8_t, 64U * 1024U> buffer{};
        while (input)
        {
            if (shouldCancel && shouldCancel())
            {
                result.error = makeError(
                    ProjectAssetInspectionErrorCode::Cancelled,
                    "Partial-track asset inspection cancelled",
                    path);
                result.bytes.clear();
                return result;
            }
            input.read(
                reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
            const auto count = input.gcount();
            if (count <= 0)
                continue;
            const auto byteCount = static_cast<std::size_t>(count);
            if (byteCount
                > maximumPartialTrackAssetBytes - result.bytes.size())
            {
                result.error = makeError(
                    ProjectAssetInspectionErrorCode::InvalidMedia,
                    "Partial-track asset exceeds the fixed byte limit",
                    path);
                result.bytes.clear();
                return result;
            }
            result.bytes.insert(
                result.bytes.end(),
                buffer.begin(),
                buffer.begin() + count);
        }
    }
    catch (const std::bad_alloc&)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ReadFailed,
            "Partial-track asset snapshot allocation failed",
            path);
        result.bytes.clear();
        return result;
    }
    if (input.bad())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ReadFailed,
            "Unable to finish reading partial-track asset",
            path);
        result.bytes.clear();
    }
    return result;
}

[[nodiscard]] bool hasAscii(
    const std::array<std::uint8_t, 12>& bytes,
    std::size_t offset,
    std::string_view expected)
{
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (bytes[offset + index]
            != static_cast<std::uint8_t>(expected[index]))
        {
            return false;
        }
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] bool hasAscii(
    const std::array<std::uint8_t, Size>& bytes,
    std::string_view expected)
{
    if (expected.size() > bytes.size())
        return false;
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (bytes[index] != static_cast<std::uint8_t>(expected[index]))
            return false;
    }
    return true;
}

template <std::size_t Size>
[[nodiscard]] std::uint16_t readU16(
    const std::array<std::uint8_t, Size>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

template <std::size_t Size>
[[nodiscard]] std::uint32_t readU32(
    const std::array<std::uint8_t, Size>& bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

template <std::size_t Size>
[[nodiscard]] bool readExact(
    std::ifstream& input,
    std::array<std::uint8_t, Size>& destination)
{
    input.read(
        reinterpret_cast<char*>(destination.data()),
        static_cast<std::streamsize>(destination.size()));
    return input.gcount()
        == static_cast<std::streamsize>(destination.size());
}

struct WavProbe
{
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string error;
    ProjectAssetInspectionErrorCode errorCode =
        ProjectAssetInspectionErrorCode::InvalidMedia;
    std::string_view encoding = pcm16WavAssetEncoding;
};

[[nodiscard]] bool spanHasAscii(
    std::span<const std::uint8_t> bytes,
    std::size_t offset,
    std::string_view expected)
{
    return offset <= bytes.size()
        && expected.size() <= bytes.size() - offset
        && std::equal(
            expected.begin(),
            expected.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            [](char left, std::uint8_t right) {
                return static_cast<std::uint8_t>(left) == right;
            });
}

[[nodiscard]] std::uint16_t spanU16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

[[nodiscard]] std::uint32_t spanU32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] WavProbe probePcm16Wav(
    std::span<const std::uint8_t> bytes,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    WavProbe result;
    const auto cancelled = [&] {
        return shouldCancel && shouldCancel();
    };
    const auto cancelProbe = [&] {
        result.error = "Asset inspection cancelled";
        result.errorCode = ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    };
    if (cancelled())
        return cancelProbe();
    if (bytes.size() < 12U)
    {
        result.error = "Unable to read WAV container header";
        result.errorCode = ProjectAssetInspectionErrorCode::ReadFailed;
        return result;
    }
    if (!spanHasAscii(bytes, 0, "RIFF")
        || !spanHasAscii(bytes, 8, "WAVE"))
    {
        result.error = "Audio asset is not a RIFF/WAVE container";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    const auto declaredEnd = static_cast<std::uint64_t>(spanU32(bytes, 4)) + 8U;
    if (declaredEnd < 12U || declaredEnd != bytes.size())
    {
        result.error = "WAV container size is inconsistent with the file";
        return result;
    }

    bool foundFormat = false;
    bool foundData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t channels = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint32_t dataBytes = 0;
    std::uint64_t offset = 12;
    while (offset + 8U <= declaredEnd)
    {
        if (cancelled())
            return cancelProbe();
        const auto chunkOffset = static_cast<std::size_t>(offset);
        const auto chunkBytes = spanU32(bytes, chunkOffset + 4U);
        const auto dataOffset = offset + 8U;
        const auto chunkEnd = dataOffset
            + static_cast<std::uint64_t>(chunkBytes);
        if (chunkEnd > declaredEnd)
        {
            result.error = "WAV chunk extends past the RIFF container";
            return result;
        }

        if (spanHasAscii(bytes, chunkOffset, "fmt "))
        {
            if (foundFormat || chunkBytes < 16U)
            {
                result.error = foundFormat
                    ? "WAV contains duplicate fmt chunks"
                    : "WAV fmt chunk is too short";
                return result;
            }
            const auto formatOffset = static_cast<std::size_t>(dataOffset);
            audioFormat = spanU16(bytes, formatOffset);
            channels = spanU16(bytes, formatOffset + 2U);
            sampleRate = spanU32(bytes, formatOffset + 4U);
            byteRate = spanU32(bytes, formatOffset + 8U);
            blockAlign = spanU16(bytes, formatOffset + 12U);
            bitsPerSample = spanU16(bytes, formatOffset + 14U);
            foundFormat = true;
        }
        else if (spanHasAscii(bytes, chunkOffset, "data"))
        {
            if (foundData)
            {
                result.error = "WAV contains duplicate data chunks";
                return result;
            }
            dataBytes = chunkBytes;
            foundData = true;
        }

        offset = chunkEnd + static_cast<std::uint64_t>(chunkBytes & 1U);
        if (offset > declaredEnd)
        {
            result.error = "WAV chunk padding extends past the RIFF container";
            return result;
        }
    }

    if (offset != declaredEnd)
    {
        result.error = "WAV container has an incomplete trailing chunk header";
        return result;
    }
    if (!foundFormat || !foundData)
    {
        result.error = !foundFormat
            ? "WAV file is missing a fmt chunk"
            : "WAV file is missing a data chunk";
        return result;
    }
    if (audioFormat != 1U || bitsPerSample != 16U)
    {
        result.error = "Audio asset must be an uncompressed PCM16 WAV";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    if (channels == 0U || channels > 2U || sampleRate == 0U)
    {
        result.error = "PCM16 WAV must have one or two channels and a positive sample rate";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    const auto expectedBlockAlign = static_cast<std::uint32_t>(channels) * 2U;
    const auto expectedByteRate = static_cast<std::uint64_t>(sampleRate)
        * expectedBlockAlign;
    if (blockAlign != expectedBlockAlign
        || expectedByteRate > std::numeric_limits<std::uint32_t>::max()
        || byteRate != expectedByteRate
        || dataBytes == 0U
        || dataBytes % expectedBlockAlign != 0U)
    {
        result.error = "PCM16 WAV layout is internally inconsistent";
        return result;
    }

    result.channelCount = channels;
    result.sampleRate = static_cast<double>(sampleRate);
    result.frameCount = dataBytes / expectedBlockAlign;
    return result;
}

// A mono or stereo WAV or AIFF sample file of any word size, which a
// DiskPlayer streams, where `pcm16` refused the
// file as no canonical PCM16 WAV.
template <typename ReadAt>
[[nodiscard]] WavProbe probeSampleFile(
    std::uint64_t size,
    ReadAt&& readAt,
    WavProbe pcm16)
{
    if (pcm16.error.empty()
        || pcm16.errorCode != ProjectAssetInspectionErrorCode::UnsupportedMedia)
    {
        return pcm16;
    }
    std::string error;
    const auto layout = talas::core::probeSampleFileLayout(
        size,
        std::forward<ReadAt>(readAt),
        error);
    if (!layout.has_value() || layout->channelCount > 2U)
    {
        pcm16.error = "Audio asset must be a WAV or AIFF sample file of one or two channels: "
            + (layout.has_value() ? std::string{"it has more channels"} : error);
        return pcm16;
    }
    WavProbe result;
    result.channelCount = layout->channelCount;
    result.sampleRate = layout->sampleRate;
    result.frameCount = layout->frameCount;
    result.encoding = sampleFileAssetEncoding;
    return result;
}

[[nodiscard]] WavProbe probeAudioAsset(
    std::span<const std::uint8_t> bytes,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    return probeSampleFile(
        bytes.size(),
        [&](std::uint64_t offset, std::span<std::uint8_t> destination) {
            if (offset > bytes.size() || destination.size() > bytes.size() - offset)
                return false;
            std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                destination.size(), destination.begin());
            return true;
        },
        probePcm16Wav(bytes, shouldCancel));
}

[[nodiscard]] WavProbe probePcm16Wav(
    const std::filesystem::path& path,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    WavProbe result;
    const auto cancelled = [&] {
        return shouldCancel && shouldCancel();
    };
    const auto cancelProbe = [&] {
        result.error = "Asset inspection cancelled";
        result.errorCode = ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    };
    if (cancelled())
        return cancelProbe();
    std::error_code fileError;
    const auto fileSize = std::filesystem::file_size(path, fileError);
    if (fileError)
    {
        result.error = "Unable to measure audio asset: "
            + fileError.message();
        result.errorCode = ProjectAssetInspectionErrorCode::ReadFailed;
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    std::array<std::uint8_t, 12> riff{};
    if (!input || !readExact(input, riff))
    {
        result.error = "Unable to read WAV container header";
        result.errorCode = ProjectAssetInspectionErrorCode::ReadFailed;
        return result;
    }
    if (!hasAscii(riff, 0, "RIFF") || !hasAscii(riff, 8, "WAVE"))
    {
        result.error = "Audio asset is not a RIFF/WAVE container";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    const auto declaredEnd = static_cast<std::uint64_t>(readU32(riff, 4)) + 8U;
    if (declaredEnd < 12U || declaredEnd != fileSize)
    {
        result.error = "WAV container size is inconsistent with the file";
        return result;
    }

    bool foundFormat = false;
    bool foundData = false;
    std::uint16_t audioFormat = 0;
    std::uint16_t channels = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t byteRate = 0;
    std::uint32_t dataBytes = 0;
    std::uint64_t offset = 12;
    while (offset + 8U <= declaredEnd)
    {
        if (cancelled())
            return cancelProbe();
        input.seekg(static_cast<std::streamoff>(offset));
        std::array<std::uint8_t, 8> chunkHeader{};
        if (!input || !readExact(input, chunkHeader))
        {
            result.error = "Unable to read WAV chunk header";
            result.errorCode = ProjectAssetInspectionErrorCode::ReadFailed;
            return result;
        }
        const auto chunkBytes = readU32(chunkHeader, 4);
        const auto dataOffset = offset + 8U;
        const auto chunkEnd = dataOffset
            + static_cast<std::uint64_t>(chunkBytes);
        if (chunkEnd > declaredEnd)
        {
            result.error = "WAV chunk extends past the RIFF container";
            return result;
        }

        if (hasAscii(chunkHeader, "fmt "))
        {
            if (foundFormat || chunkBytes < 16U)
            {
                result.error = foundFormat
                    ? "WAV contains duplicate fmt chunks"
                    : "WAV fmt chunk is too short";
                return result;
            }
            std::array<std::uint8_t, 16> format{};
            if (!readExact(input, format))
            {
                result.error = "Unable to read WAV format metadata";
                result.errorCode = ProjectAssetInspectionErrorCode::ReadFailed;
                return result;
            }
            audioFormat = readU16(format, 0);
            channels = readU16(format, 2);
            sampleRate = readU32(format, 4);
            byteRate = readU32(format, 8);
            blockAlign = readU16(format, 12);
            bitsPerSample = readU16(format, 14);
            foundFormat = true;
        }
        else if (hasAscii(chunkHeader, "data"))
        {
            if (foundData)
            {
                result.error = "WAV contains duplicate data chunks";
                return result;
            }
            dataBytes = chunkBytes;
            foundData = true;
        }

        offset = chunkEnd + static_cast<std::uint64_t>(chunkBytes & 1U);
        if (offset > declaredEnd)
        {
            result.error = "WAV chunk padding extends past the RIFF container";
            return result;
        }
    }

    if (offset != declaredEnd)
    {
        result.error = "WAV container has an incomplete trailing chunk header";
        return result;
    }

    if (!foundFormat || !foundData)
    {
        result.error = !foundFormat
            ? "WAV file is missing a fmt chunk"
            : "WAV file is missing a data chunk";
        return result;
    }
    if (audioFormat != 1U || bitsPerSample != 16U)
    {
        result.error = "Audio asset must be an uncompressed PCM16 WAV";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    if (channels == 0U || channels > 2U || sampleRate == 0U)
    {
        result.error = "PCM16 WAV must have one or two channels and a positive sample rate";
        result.errorCode = ProjectAssetInspectionErrorCode::UnsupportedMedia;
        return result;
    }
    const auto expectedBlockAlign = static_cast<std::uint32_t>(channels) * 2U;
    const auto expectedByteRate = static_cast<std::uint64_t>(sampleRate)
        * expectedBlockAlign;
    if (blockAlign != expectedBlockAlign
        || expectedByteRate > std::numeric_limits<std::uint32_t>::max()
        || byteRate != expectedByteRate
        || dataBytes == 0U
        || dataBytes % expectedBlockAlign != 0U)
    {
        result.error = "PCM16 WAV layout is internally inconsistent";
        return result;
    }

    result.channelCount = channels;
    result.sampleRate = static_cast<double>(sampleRate);
    result.frameCount = dataBytes / expectedBlockAlign;
    return result;
}

[[nodiscard]] WavProbe probeAudioAsset(
    const std::filesystem::path& path,
    const ProjectAssetInspectionCancellationCheck& shouldCancel)
{
    auto pcm16 = probePcm16Wav(path, shouldCancel);
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    std::ifstream input(path, std::ios::binary);
    if (sizeError || !input)
        return pcm16;
    return probeSampleFile(
        size,
        [&](std::uint64_t offset, std::span<std::uint8_t> destination) {
            input.clear();
            input.seekg(static_cast<std::streamoff>(offset));
            input.read(
                reinterpret_cast<char*>(destination.data()),
                static_cast<std::streamsize>(destination.size()));
            return input.gcount()
                == static_cast<std::streamsize>(destination.size());
        },
        std::move(pcm16));
}

}

bool isCanonicalProjectAssetContentHash(std::string_view hash) noexcept
{
    if (hash.size() != 64U)
        return false;
    for (const auto character : hash)
    {
        if (!((character >= '0' && character <= '9')
              || (character >= 'a' && character <= 'f')))
        {
            return false;
        }
    }
    return true;
}

std::string calculateProjectAssetSha256(
    std::span<const std::uint8_t> bytes)
{
    return calculateProjectAssetSha256(
               bytes,
               {})
        .contentHash;
}

ProjectAssetHashResult calculateProjectAssetSha256(
    std::span<const std::uint8_t> bytes,
    ProjectAssetInspectionCancellationCheck shouldCancel)
{
    ProjectAssetHashResult result;
    Sha256 state;
    constexpr std::size_t hashChunkBytes =
        64U * 1024U;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (shouldCancel && shouldCancel())
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::Cancelled,
                "Asset hashing cancelled",
                {});
            return result;
        }
        const auto count = std::min(
            hashChunkBytes,
            bytes.size() - offset);
        if (!state.append(
                bytes.data() + offset,
                count))
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::ReadFailed,
                "Asset is too large to hash safely",
                {});
            return result;
        }
        offset += count;
    }
    if (shouldCancel && shouldCancel())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Cancelled,
            "Asset hashing cancelled",
            {});
        return result;
    }
    result.contentHash = state.finish();
    return result;
}

std::optional<ProjectAssetInspectionError>
validateProjectAssetManifest(const ProjectAsset& asset)
{
    if (asset.id.empty() || asset.path.empty())
    {
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Project asset id and path must not be empty",
            {});
    }
    switch (asset.kind)
    {
    case ProjectAssetKind::Table:
    case ProjectAssetKind::Binary:
    case ProjectAssetKind::Audio:
    case ProjectAssetKind::PartialSpectrum:
        break;
    default:
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Project asset kind is invalid",
            {});
    }
    switch (asset.disposition)
    {
    case ProjectAssetDisposition::Referenced:
    case ProjectAssetDisposition::Embedded:
        break;
    default:
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Project asset disposition is invalid",
            {});
    }
    if ((!asset.contentHash.empty()
         && !isCanonicalProjectAssetContentHash(asset.contentHash))
        || asset.generation == 0U
        || asset.generation > maximumProjectAssetJsonInteger
        || asset.frameCount > maximumProjectAssetJsonInteger
        || !std::isfinite(asset.sampleRate)
        || asset.sampleRate < 0.0
        || asset.sampleRate
            > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()))
    {
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Project asset hash, generation, or media counts are invalid",
            {});
    }
    if (asset.kind != ProjectAssetKind::Audio
        && asset.kind != ProjectAssetKind::PartialSpectrum
        && (asset.channelCount != 0U
            || asset.sampleRate != 0.0
            || asset.frameCount != 0U
            || !asset.encoding.empty()))
    {
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Table and binary assets cannot declare media metadata",
            {});
    }
    if (asset.kind == ProjectAssetKind::PartialSpectrum
        && (asset.channelCount != 0U
            || (asset.sampleRate != 0.0
                && asset.sampleRate
                    > maximumPartialTrackSampleRate)
            || (!asset.encoding.empty()
                && asset.encoding != partialTrackAssetEncoding)))
    {
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Partial-spectrum assets require zero channels and talas.partial-tracks-v1 encoding",
            {});
    }
    if (asset.disposition == ProjectAssetDisposition::Embedded
        && asset.contentHash.empty())
    {
        return makeError(
            ProjectAssetInspectionErrorCode::InvalidManifest,
            "Embedded assets require a verified SHA-256 hash",
            {});
    }
    return std::nullopt;
}

std::optional<ProjectAssetInspectionError>
validateProjectScoreRenderArtifacts(const ProjectDocument& document)
{
    std::unordered_map<std::string_view, const ProjectAsset*> assets;
    assets.reserve(document.assets.size());
    for (const auto& asset : document.assets)
        assets.try_emplace(asset.id, &asset);

    for (const auto& clip : document.timeline.score.clips)
    {
        if (!clip.renderArtifact.has_value())
            continue;
        if (document.formatVersion
            < scoreClipRenderArtifactProjectFormatVersion)
        {
            return makeError(
                ProjectAssetInspectionErrorCode::InvalidManifest,
                "Score clip render artifacts require project format 33",
                clip.clipId);
        }
        const auto& artifact = *clip.renderArtifact;
        for (const auto& assetId : artifact.channelAssetIds)
        {
            const auto found = assets.find(assetId);
            if (found == assets.end())
            {
                return makeError(
                    ProjectAssetInspectionErrorCode::InvalidManifest,
                    "Score clip render artifact references missing asset '"
                        + assetId + "'",
                    clip.clipId);
            }
            const auto& asset = *found->second;
            if (asset.kind != ProjectAssetKind::Audio
                || asset.channelCount != 1
                || asset.sampleRate
                    != static_cast<double>(artifact.sampleRate)
                || asset.frameCount != artifact.frameCount
                || (asset.encoding != pcm16WavAssetEncoding
                    && asset.encoding != sampleFileAssetEncoding)
                || asset.contentHash.empty()
                || asset.generation == 0)
            {
                return makeError(
                    ProjectAssetInspectionErrorCode::InvalidManifest,
                    "Score clip render artifact asset '" + assetId
                        + "' must be verified mono PCM16 WAV with exact sample rate and frame count",
                    clip.clipId);
            }
        }
    }
    return std::nullopt;
}

ProjectAssetHashResult calculateProjectAssetSha256(
    const std::filesystem::path& path,
    ProjectAssetInspectionCancellationCheck shouldCancel)
{
    ProjectAssetHashResult result;
    if (shouldCancel && shouldCancel())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Cancelled,
            "Asset inspection cancelled",
            path);
        return result;
    }
    if (path.empty())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Asset path cannot be empty",
            path);
        return result;
    }
    std::error_code statusError;
    const auto status = std::filesystem::status(path, statusError);
    if (statusError)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Unavailable,
            "Unable to inspect asset: " + statusError.message(),
            path);
        return result;
    }
    if (!std::filesystem::is_regular_file(status))
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::NotRegularFile,
            "Asset is not a regular file",
            path);
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ReadFailed,
            "Unable to open asset for hashing",
            path);
        return result;
    }
    Sha256 state;
    std::array<std::uint8_t, 64U * 1024U> buffer{};
    while (input)
    {
        if (shouldCancel && shouldCancel())
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::Cancelled,
                "Asset inspection cancelled",
                path);
            return result;
        }
        input.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0
            && !state.append(
                buffer.data(),
                static_cast<std::size_t>(count)))
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::ReadFailed,
                "Asset is too large to hash safely",
                path);
            return result;
        }
    }
    if (input.bad())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ReadFailed,
            "Unable to finish reading asset for hashing",
            path);
        return result;
    }
    result.contentHash = state.finish();
    return result;
}

ProjectAssetInspectionResult inspectProjectAsset(
    const ProjectAsset& manifest,
    const std::filesystem::path& resolvedPath,
    ProjectAssetInspectionCancellationCheck shouldCancel)
{
    ProjectAssetInspectionResult result;
    result.asset = manifest;
    if (const auto invalid = validateProjectAssetManifest(manifest);
        invalid.has_value())
    {
        result.error = *invalid;
        result.error->path = pathToUtf8(resolvedPath);
        return result;
    }

    if (manifest.kind == ProjectAssetKind::Audio
        && manifest.provenance.starts_with(
            diskCacheCaptureProvenanceNamespace))
    {
        constexpr auto maximumCapturedWavBytes = 44U
            + 2U * static_cast<std::size_t>(maximumDiskCacheCaptureFrames);
        auto snapshot = readDirectBoundedBytes(
            resolvedPath,
            maximumCapturedWavBytes,
            shouldCancel);
        if (snapshot.error.has_value())
        {
            result.error = std::move(snapshot.error);
            return result;
        }
        auto inspected = inspectProjectAsset(
            manifest,
            snapshot.bytes,
            resolvedPath,
            std::move(shouldCancel));
        if (inspected.ok())
            inspected.inspectedBytes = std::move(snapshot.bytes);
        return inspected;
    }

    if (manifest.kind == ProjectAssetKind::PartialSpectrum)
    {
        auto snapshot = readBoundedPartialTrackBytes(
            resolvedPath,
            shouldCancel);
        if (snapshot.error.has_value())
        {
            result.error = std::move(snapshot.error);
            return result;
        }
        return inspectProjectAsset(
            manifest,
            snapshot.bytes,
            resolvedPath,
            std::move(shouldCancel));
    }

    const auto hash = calculateProjectAssetSha256(
        resolvedPath,
        shouldCancel);
    if (!hash.ok())
    {
        result.error = hash.error;
        return result;
    }
    if (!manifest.contentHash.empty()
        && manifest.contentHash != hash.contentHash)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset content does not match the accepted SHA-256 hash",
            resolvedPath);
        return result;
    }
    result.asset.contentHash = hash.contentHash;

    if (manifest.kind != ProjectAssetKind::Audio)
        return result;

    if (shouldCancel && shouldCancel())
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::Cancelled,
            "Asset inspection cancelled",
            resolvedPath);
        return result;
    }

    const auto media = probeAudioAsset(resolvedPath, shouldCancel);
    if (!media.error.empty())
    {
        result.error = makeError(
            media.errorCode,
            media.error,
            resolvedPath);
        return result;
    }
    const auto stableHash = calculateProjectAssetSha256(
        resolvedPath,
        shouldCancel);
    if (!stableHash.ok())
    {
        result.error = stableHash.error;
        return result;
    }
    if (stableHash.contentHash != result.asset.contentHash)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Audio asset changed during local inspection",
            resolvedPath);
        return result;
    }
    const auto expectedEncodingMatches = manifest.encoding.empty()
        || manifest.encoding == media.encoding;
    if ((manifest.channelCount != 0U
         && manifest.channelCount != media.channelCount)
        || (manifest.sampleRate != 0.0
            && manifest.sampleRate != media.sampleRate)
        || (manifest.frameCount != 0U
            && manifest.frameCount != media.frameCount)
        || !expectedEncodingMatches)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::MediaMismatch,
            "Audio asset media shape does not match the accepted manifest",
            resolvedPath);
        return result;
    }
    result.asset.channelCount = media.channelCount;
    result.asset.sampleRate = media.sampleRate;
    result.asset.frameCount = media.frameCount;
    result.asset.encoding = std::string{media.encoding};
    return result;
}

ProjectAssetInspectionResult inspectProjectAsset(
    const ProjectAsset& manifest,
    std::span<const std::uint8_t> bytes,
    const std::filesystem::path& diagnosticPath,
    ProjectAssetInspectionCancellationCheck shouldCancel)
{
    ProjectAssetInspectionResult result;
    result.asset = manifest;
    if (const auto invalid = validateProjectAssetManifest(manifest);
        invalid.has_value())
    {
        result.error = *invalid;
        result.error->path = pathToUtf8(diagnosticPath);
        return result;
    }

    const auto hash = calculateProjectAssetSha256(bytes, shouldCancel);
    if (!hash.ok())
    {
        result.error = hash.error;
        result.error->path = pathToUtf8(diagnosticPath);
        return result;
    }
    if (!manifest.contentHash.empty()
        && manifest.contentHash != hash.contentHash)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::ContentMismatch,
            "Asset content does not match the accepted SHA-256 hash",
            diagnosticPath);
        return result;
    }
    result.asset.contentHash = hash.contentHash;

    if (manifest.kind != ProjectAssetKind::Audio
        && manifest.kind != ProjectAssetKind::PartialSpectrum)
        return result;
    if (manifest.kind == ProjectAssetKind::PartialSpectrum)
    {
        if (shouldCancel && shouldCancel())
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::Cancelled,
                "Partial-track asset inspection cancelled",
                diagnosticPath);
            return result;
        }
        const auto decoded = decodePartialTrackAsset(
            bytes,
            shouldCancel);
        if (!decoded.ok())
        {
            result.error = makeError(
                decoded.error->code
                        == PartialTrackAssetErrorCode::Cancelled
                    ? ProjectAssetInspectionErrorCode::Cancelled
                    : ProjectAssetInspectionErrorCode::InvalidMedia,
                decoded.error->message,
                diagnosticPath);
            return result;
        }
        if ((manifest.sampleRate != 0.0
                && manifest.sampleRate != decoded.asset.sampleRate)
            || (manifest.frameCount != 0U
                && manifest.frameCount
                    != decoded.asset.sourceFrameCount)
            || (!manifest.encoding.empty()
                && manifest.encoding != partialTrackAssetEncoding))
        {
            result.error = makeError(
                ProjectAssetInspectionErrorCode::MediaMismatch,
                "Partial-track asset shape does not match the accepted manifest",
                diagnosticPath);
            return result;
        }
        result.asset.channelCount = 0;
        result.asset.sampleRate = decoded.asset.sampleRate;
        result.asset.frameCount = decoded.asset.sourceFrameCount;
        result.asset.encoding = std::string{partialTrackAssetEncoding};
        return result;
    }
    const auto media = probeAudioAsset(bytes, shouldCancel);
    if (!media.error.empty())
    {
        result.error = makeError(
            media.errorCode,
            media.error,
            diagnosticPath);
        return result;
    }
    const auto expectedEncodingMatches = manifest.encoding.empty()
        || manifest.encoding == media.encoding;
    if ((manifest.channelCount != 0U
         && manifest.channelCount != media.channelCount)
        || (manifest.sampleRate != 0.0
            && manifest.sampleRate != media.sampleRate)
        || (manifest.frameCount != 0U
            && manifest.frameCount != media.frameCount)
        || !expectedEncodingMatches)
    {
        result.error = makeError(
            ProjectAssetInspectionErrorCode::MediaMismatch,
            "Audio asset media shape does not match the accepted manifest",
            diagnosticPath);
        return result;
    }
    result.asset.channelCount = media.channelCount;
    result.asset.sampleRate = media.sampleRate;
    result.asset.frameCount = media.frameCount;
    result.asset.encoding = std::string{media.encoding};
    return result;
}
}
