#include "runtime/component_activation/ComponentActivationAudioSourceFreezer.h"

#include "talas/audio/Pcm16WaveSpan.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/runtime/ComponentActivationProtocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <fstream>
#include <new>
#include <span>
#include <system_error>
#include <vector>

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace talas::runtime::component_activation
{
namespace
{
[[nodiscard]] std::optional<detail::ComponentActivationPcm16WaveShapeV1>
parsePcm16Wave(
    std::span<const std::uint8_t> bytes) noexcept
{
    const auto parsed = talas::audio::parseStrictPcm16Wave(
        bytes, maximumComponentActivationAudioSourceSampleRateV1);
    if (!parsed.ok())
        return std::nullopt;
    return detail::ComponentActivationPcm16WaveShapeV1{
        parsed.layout.channelCount,
        parsed.layout.sampleRate,
        parsed.layout.frameCount,
    };
}

[[nodiscard]] bool validSnapshot(
    const talas::core::ResourceSnapshot& snapshot) noexcept
{
    if (snapshot.kind != talas::core::ResourceSnapshotKind::Audio
        || snapshot.id.empty()
        || snapshot.id.size()
            > maximumComponentActivationDescriptorIdBytesV1
        || snapshot.generation == 0
        || !talas::project::isCanonicalProjectAssetContentHash(
            snapshot.contentHash)
        || snapshot.resolvedPath.empty()
        || !snapshot.tableSamples.empty()
        || !snapshot.tableRightSamples.empty()
        || !snapshot.binaryBytes.empty()
        || (snapshot.provenance.starts_with(
                talas::project::diskCacheCaptureProvenanceNamespace)
            != !snapshot.audioBytes.empty())
        || (!snapshot.audioBytes.empty()
            && snapshot.audioBytes.size()
                > 44U + 2U * static_cast<std::size_t>(
                    talas::project::maximumDiskCacheCaptureFrames))
        || (snapshot.channelCount != 1 && snapshot.channelCount != 2)
        || !std::isfinite(snapshot.sampleRate)
        || snapshot.sampleRate < 1.0
        || snapshot.sampleRate
            > maximumComponentActivationAudioSourceSampleRateV1
        || std::floor(snapshot.sampleRate) != snapshot.sampleRate
        || snapshot.frameCount == 0
        || snapshot.encoding != talas::project::pcm16WavAssetEncoding)
    {
        return false;
    }
    for (const auto character : snapshot.id)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x21U || byte > 0x7eU)
            return false;
    }
    return true;
}

[[nodiscard]] ComponentActivationAudioSourceFreezeResultV1 failed(
    ComponentActivationAudioSourceFreezeCodeV1 code) noexcept
{
    return {std::nullopt, code};
}

#if defined(__APPLE__) || defined(__linux__) || defined(__unix__)
class FileDescriptor
{
public:
    explicit FileDescriptor(int value) noexcept : value_(value) {}
    ~FileDescriptor()
    {
        if (value_ >= 0)
            ::close(value_);
    }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_ = -1;
};

[[nodiscard]] bool sameTimes(
    const struct stat& left,
    const struct stat& right) noexcept
{
#if defined(__APPLE__)
    return left.st_mtimespec.tv_sec == right.st_mtimespec.tv_sec
        && left.st_mtimespec.tv_nsec == right.st_mtimespec.tv_nsec
        && left.st_ctimespec.tv_sec == right.st_ctimespec.tv_sec
        && left.st_ctimespec.tv_nsec == right.st_ctimespec.tv_nsec;
#else
    return left.st_mtim.tv_sec == right.st_mtim.tv_sec
        && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec
        && left.st_ctim.tv_sec == right.st_ctim.tv_sec
        && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
#endif
}

[[nodiscard]] ComponentActivationAudioSourceFreezeCodeV1 readExactFile(
    const std::filesystem::path& path,
    const talas::project::ProjectAssetInspectionCancellationCheck& shouldCancel,
    std::vector<std::uint8_t>& bytes) noexcept
{
    std::error_code linkError;
    const auto linkStatus = std::filesystem::symlink_status(path, linkError);
    if (linkError)
    {
        return linkError == std::errc::no_such_file_or_directory
            ? ComponentActivationAudioSourceFreezeCodeV1::Unavailable
            : ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    }
    if (std::filesystem::is_symlink(linkStatus)
        || !std::filesystem::is_regular_file(linkStatus))
    {
        return ComponentActivationAudioSourceFreezeCodeV1::NotRegularFile;
    }
    if (shouldCancel && shouldCancel())
        return ComponentActivationAudioSourceFreezeCodeV1::Cancelled;

    const auto descriptor = ::open(
        path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
    {
        if (errno == ENOENT)
            return ComponentActivationAudioSourceFreezeCodeV1::Unavailable;
        if (errno == ELOOP)
            return ComponentActivationAudioSourceFreezeCodeV1::NotRegularFile;
        return ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    }
    FileDescriptor file{descriptor};
    struct stat before{};
    if (::fstat(file.get(), &before) != 0)
        return ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    if (!S_ISREG(before.st_mode))
        return ComponentActivationAudioSourceFreezeCodeV1::NotRegularFile;
    if (before.st_size
            < static_cast<off_t>(minimumComponentActivationAudioSourceBytesV1)
        || static_cast<std::uint64_t>(before.st_size)
            > maximumComponentActivationAudioSourceBytesV1)
    {
        return ComponentActivationAudioSourceFreezeCodeV1::MediaInvalid;
    }
    try
    {
        bytes.resize(static_cast<std::size_t>(before.st_size));
    }
    catch (const std::bad_alloc&)
    {
        return ComponentActivationAudioSourceFreezeCodeV1::AllocationFailed;
    }

    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (shouldCancel && shouldCancel())
            return ComponentActivationAudioSourceFreezeCodeV1::Cancelled;
        const auto count = std::min<std::size_t>(
            64U * 1024U, bytes.size() - offset);
        const auto readCount = ::read(
            file.get(), bytes.data() + offset, count);
        if (readCount < 0 && errno == EINTR)
            continue;
        if (readCount <= 0)
            return ComponentActivationAudioSourceFreezeCodeV1::SourceChanged;
        offset += static_cast<std::size_t>(readCount);
    }
    std::uint8_t extra = 0;
    const auto extraCount = ::read(file.get(), &extra, 1);
    if (extraCount < 0 && errno != EINTR)
        return ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    if (extraCount != 0)
        return ComponentActivationAudioSourceFreezeCodeV1::SourceChanged;
    if (shouldCancel && shouldCancel())
        return ComponentActivationAudioSourceFreezeCodeV1::Cancelled;

    struct stat after{};
    struct stat currentPath{};
    if (::fstat(file.get(), &after) != 0
        || ::lstat(path.c_str(), &currentPath) != 0
        || !S_ISREG(currentPath.st_mode)
        || before.st_dev != after.st_dev
        || before.st_ino != after.st_ino
        || before.st_size != after.st_size
        || !sameTimes(before, after)
        || after.st_dev != currentPath.st_dev
        || after.st_ino != currentPath.st_ino
        || after.st_size != currentPath.st_size
        || !sameTimes(after, currentPath))
    {
        return ComponentActivationAudioSourceFreezeCodeV1::SourceChanged;
    }
    return ComponentActivationAudioSourceFreezeCodeV1::Ready;
}
#else
[[nodiscard]] ComponentActivationAudioSourceFreezeCodeV1 readExactFile(
    const std::filesystem::path& path,
    const talas::project::ProjectAssetInspectionCancellationCheck& shouldCancel,
    std::vector<std::uint8_t>& bytes) noexcept
{
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(path, statusError);
    if (statusError)
        return ComponentActivationAudioSourceFreezeCodeV1::Unavailable;
    if (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))
    {
        return ComponentActivationAudioSourceFreezeCodeV1::NotRegularFile;
    }
    const auto size = std::filesystem::file_size(path, statusError);
    if (statusError)
        return ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    if (size < minimumComponentActivationAudioSourceBytesV1
        || size > maximumComponentActivationAudioSourceBytesV1)
    {
        return ComponentActivationAudioSourceFreezeCodeV1::MediaInvalid;
    }
    try
    {
        bytes.resize(static_cast<std::size_t>(size));
    }
    catch (const std::bad_alloc&)
    {
        return ComponentActivationAudioSourceFreezeCodeV1::AllocationFailed;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return ComponentActivationAudioSourceFreezeCodeV1::ReadFailed;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (shouldCancel && shouldCancel())
            return ComponentActivationAudioSourceFreezeCodeV1::Cancelled;
        const auto count = std::min<std::size_t>(
            64U * 1024U, bytes.size() - offset);
        input.read(
            reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count))
            return ComponentActivationAudioSourceFreezeCodeV1::SourceChanged;
        offset += count;
    }
    return ComponentActivationAudioSourceFreezeCodeV1::Ready;
}
#endif
}

std::optional<detail::ComponentActivationPcm16WaveShapeV1>
parseComponentActivationPcm16WaveV1(
    std::span<const std::uint8_t> bytes) noexcept
{
    return parsePcm16Wave(bytes);
}

ComponentActivationFrozenAudioSourceV1::ComponentActivationFrozenAudioSourceV1(
    std::shared_ptr<const detail::ComponentActivationFrozenAudioSourceDataV1>
        data) noexcept
    : data_(std::move(data))
{
}

ComponentActivationFrozenAudioSourceV1::~ComponentActivationFrozenAudioSourceV1()
    = default;

ComponentActivationFrozenAudioSourceV1::ComponentActivationFrozenAudioSourceV1(
    ComponentActivationFrozenAudioSourceV1&&) noexcept = default;

ComponentActivationFrozenAudioSourceV1&
ComponentActivationFrozenAudioSourceV1::operator=(
    ComponentActivationFrozenAudioSourceV1&&) noexcept = default;

bool ComponentActivationFrozenAudioSourceV1::valid() const noexcept
{
    return data_ != nullptr;
}

std::uint64_t ComponentActivationFrozenAudioSourceV1::sourceByteCount() const
    noexcept
{
    return data_ == nullptr
        ? 0
        : static_cast<std::uint64_t>(data_->bytes.size());
}

ComponentActivationAudioSourceFreezeResultV1
ComponentActivationAudioSourceFreezerV1::freeze(
    const talas::core::ResourceSnapshot& snapshot,
    talas::project::ProjectAssetInspectionCancellationCheck shouldCancel) noexcept
{
    try
    {
        if (!validSnapshot(snapshot))
        {
            return failed(
                ComponentActivationAudioSourceFreezeCodeV1::SnapshotInvalid);
        }
        std::vector<std::uint8_t> bytes = snapshot.audioBytes;
        if (bytes.empty())
        {
            const auto readCode = readExactFile(
                snapshot.resolvedPath, shouldCancel, bytes);
            if (readCode != ComponentActivationAudioSourceFreezeCodeV1::Ready)
                return failed(readCode);
        }
        const auto hash = talas::project::calculateProjectAssetSha256(
            bytes, shouldCancel);
        if (!hash.ok())
        {
            return failed(
                hash.error->code
                        == talas::project::ProjectAssetInspectionErrorCode::
                            Cancelled
                    ? ComponentActivationAudioSourceFreezeCodeV1::Cancelled
                    : ComponentActivationAudioSourceFreezeCodeV1::ReadFailed);
        }
        if (hash.contentHash != snapshot.contentHash)
        {
            return failed(
                ComponentActivationAudioSourceFreezeCodeV1::ContentMismatch);
        }
        const auto shape = parseComponentActivationPcm16WaveV1(bytes);
        if (!shape.has_value()
            || shape->channelCount != snapshot.channelCount
            || shape->sampleRate != snapshot.sampleRate
            || shape->frameCount != snapshot.frameCount)
        {
            return failed(
                ComponentActivationAudioSourceFreezeCodeV1::MediaInvalid);
        }
        if (shouldCancel && shouldCancel())
        {
            return failed(
                ComponentActivationAudioSourceFreezeCodeV1::Cancelled);
        }
        auto data = std::make_shared<
            detail::ComponentActivationFrozenAudioSourceDataV1>();
        data->id = snapshot.id;
        data->generation = snapshot.generation;
        data->sourceContentDigest = calculateComponentActivationByteDigestV1(
            bytes);
        data->bytes = std::move(bytes);
        data->channelCount = shape->channelCount;
        data->sampleRate = shape->sampleRate;
        data->frameCount = shape->frameCount;
        return {
            std::optional<ComponentActivationFrozenAudioSourceV1>{
                ComponentActivationFrozenAudioSourceV1{std::move(data)}},
            ComponentActivationAudioSourceFreezeCodeV1::Ready,
        };
    }
    catch (const std::bad_alloc&)
    {
        return failed(ComponentActivationAudioSourceFreezeCodeV1::AllocationFailed);
    }
    catch (...)
    {
        return failed(ComponentActivationAudioSourceFreezeCodeV1::ReadFailed);
    }
}
}
