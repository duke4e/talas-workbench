#include "talas/audio/ResourceLoader.h"

#include "talas/audio/WavReader.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace talas::audio
{
namespace
{
constexpr std::size_t resourceReadChunkBytes = 64U * 1024U;

struct ResourceBytesReadResult
{
    std::vector<std::uint8_t> bytes;
    std::string error;
    bool cancelled = false;
};

ResourceBytesReadResult readResourceBytes(
    const std::filesystem::path& path,
    std::string_view resourceDescription,
    ResourceLoadCancellationCheck shouldCancel,
    std::size_t maximumBytes =
        std::numeric_limits<std::size_t>::max())
{
    ResourceBytesReadResult result;
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.error = "failed to read "
            + std::string(resourceDescription) + ": "
            + talas::project::pathToUtf8(path);
        return result;
    }

    std::array<char, resourceReadChunkBytes> buffer{};
    while (input)
    {
        if (shouldCancel && shouldCancel())
        {
            result.cancelled = true;
            result.error = "resource loading cancelled";
            result.bytes.clear();
            return result;
        }
        input.read(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0)
        {
            const auto byteCount = static_cast<std::size_t>(count);
            if (byteCount > maximumBytes - result.bytes.size())
            {
                result.error = std::string(resourceDescription)
                    + " exceeds its fixed byte limit";
                result.bytes.clear();
                return result;
            }
            const auto begin = reinterpret_cast<
                const std::uint8_t*>(buffer.data());
            try
            {
                result.bytes.insert(
                    result.bytes.end(),
                    begin,
                    begin + byteCount);
            }
            catch (const std::bad_alloc&)
            {
                result.error = std::string(resourceDescription)
                    + " allocation failed";
                result.bytes.clear();
                return result;
            }
        }
    }
    if (input.bad())
    {
        result.error = "failed while reading "
            + std::string(resourceDescription) + ": "
            + talas::project::pathToUtf8(path);
        result.bytes.clear();
    }
    return result;
}

bool isNumericTableSeparator(char character)
{
    return std::isspace(static_cast<unsigned char>(character)) != 0
        || character == ','
        || character == ';';
}

std::optional<std::vector<double>> parseNumericTable(
    std::string_view text,
    std::string& error,
    bool& cancelled,
    const ResourceLoadCancellationCheck& shouldCancel)
{
    constexpr std::size_t cancellationStride = 4096U;
    constexpr std::size_t maximumNumericTokenBytes = 1024U;
    std::vector<double> samples;
    const char* const begin = text.data();
    const char* cursor = begin;
    const char* const end = begin + text.size();
    std::size_t nextCancellationOffset = 0;
    const auto cancellationRequested = [&] {
        const auto offset = static_cast<std::size_t>(
            cursor - begin);
        if (offset < nextCancellationOffset)
            return false;
        nextCancellationOffset = offset + cancellationStride;
        if (!shouldCancel || !shouldCancel())
            return false;
        cancelled = true;
        error = "resource loading cancelled";
        return true;
    };
    while (cursor < end)
    {
        while (cursor < end)
        {
            if (cancellationRequested())
                return std::nullopt;
            if (*cursor == '#')
            {
                while (cursor < end && *cursor != '\n')
                {
                    ++cursor;
                    if (cancellationRequested())
                        return std::nullopt;
                }
                continue;
            }
            if (!isNumericTableSeparator(*cursor))
                break;
            ++cursor;
        }
        if (cursor >= end)
            break;

        const auto* const tokenBegin = cursor;
        auto* tokenEnd = cursor;
        while (tokenEnd < end
               && !isNumericTableSeparator(*tokenEnd)
               && *tokenEnd != '#')
        {
            ++tokenEnd;
            cursor = tokenEnd;
            if (cancellationRequested())
                return std::nullopt;
            if (static_cast<std::size_t>(
                    tokenEnd - tokenBegin)
                > maximumNumericTokenBytes)
            {
                break;
            }
        }
        cursor = tokenEnd;
        const auto tokenLength = static_cast<std::size_t>(
            tokenEnd - tokenBegin);
        if (tokenLength > maximumNumericTokenBytes)
        {
            error = "numeric table token exceeds 1024 bytes at byte "
                + std::to_string(static_cast<std::size_t>(tokenBegin - begin));
            return std::nullopt;
        }
        const std::string token{tokenBegin, tokenEnd};
        char* parsedEnd = nullptr;
        const auto value = std::strtod(
            token.c_str(),
            &parsedEnd);
        if (parsedEnd == token.c_str()
            || parsedEnd
                != token.c_str()
                    + token.size())
        {
            error = "numeric table parse error at byte "
                + std::to_string(static_cast<std::size_t>(tokenBegin - begin));
            return std::nullopt;
        }
        if (!std::isfinite(value))
        {
            error = "numeric table contains non-finite value at byte "
                + std::to_string(static_cast<std::size_t>(tokenBegin - begin));
            return std::nullopt;
        }
        samples.push_back(value);
    }

    if (samples.empty())
    {
        error = "numeric table contains no samples";
        return std::nullopt;
    }
    return samples;
}

}

bool isSampleFilePath(const std::filesystem::path& path)
{
    auto extension = talas::project::pathToUtf8(path.extension());
    for (auto& character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    // WAVE, AIFF, IRCAM/MTU, SD-I and SD-II.
    return extension == ".wav" || extension == ".wave" || extension == ".aif"
        || extension == ".aiff" || extension == ".aifc" || extension == ".sf" || extension == ".sd1"
        || extension == ".sd2";
}

TableResourceLoadResult loadTableResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel)
{
    if (isSampleFilePath(path))
    {
        auto read = readSampleFile(path, shouldCancel);
        if (!read.ok())
        {
            TableResourceLoadResult result;
            result.error = std::move(read.error);
            result.cancelled = read.cancelled;
            return result;
        }
        if (read.channels.empty() || read.channels.size() > 2)
            return {{}, "WAV table resources must have one or two channels"};
        TableResourceLoadResult result;
        result.samples = std::move(read.channels.front());
        if (read.channels.size() == 2)
            result.rightSamples = std::move(read.channels[1]);
        result.contentHash = std::move(read.contentHash);
        result.sourceSampleRate = read.sampleRate;
        result.forwardLoop = read.forwardLoop;
        result.samplePitch = read.samplePitch;
        result.keyRange = read.keyRange;
        result.markers = std::move(read.markers);
        return result;
    }

    auto read = readResourceBytes(
        path,
        "numeric table file",
        shouldCancel);
    if (!read.error.empty() || read.cancelled)
    {
        TableResourceLoadResult result;
        result.error = std::move(read.error);
        result.cancelled = read.cancelled;
        return result;
    }
    const std::string parseText{
        reinterpret_cast<const char*>(read.bytes.data()),
        read.bytes.size(),
    };
    std::string error;
    bool cancelled = false;
    auto samples = parseNumericTable(
        parseText,
        error,
        cancelled,
        shouldCancel);
    if (!samples.has_value())
    {
        TableResourceLoadResult result;
        result.error = std::move(error);
        result.cancelled = cancelled;
        return result;
    }
    auto contentHash =
        talas::project::calculateProjectAssetSha256(
            read.bytes,
            shouldCancel);
    if (!contentHash.ok())
    {
        TableResourceLoadResult result;
        result.error = contentHash.error->message;
        result.cancelled =
            contentHash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    return {
        std::move(*samples),
        {},
        std::move(contentHash.contentHash),
    };
}

BinaryResourceLoadResult loadBinaryResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel)
{
    auto read = readResourceBytes(
        path,
        "binary resource file",
        shouldCancel);
    if (!read.error.empty() || read.cancelled)
    {
        BinaryResourceLoadResult result;
        result.error = std::move(read.error);
        result.cancelled = read.cancelled;
        return result;
    }
    if (read.bytes.empty())
        return {{}, "binary resource file is empty"};
    auto contentHash =
        talas::project::calculateProjectAssetSha256(
            read.bytes,
            shouldCancel);
    if (!contentHash.ok())
    {
        BinaryResourceLoadResult result;
        result.error = contentHash.error->message;
        result.cancelled =
            contentHash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    return {
        std::move(read.bytes),
        {},
        std::move(contentHash.contentHash),
    };
}

BinaryResourceLoadResult loadPartialTrackResource(
    const std::filesystem::path& path,
    ResourceLoadCancellationCheck shouldCancel)
{
    auto read = readResourceBytes(
        path,
        "partial-track resource file",
        shouldCancel,
        talas::project::maximumPartialTrackAssetBytes);
    if (!read.error.empty() || read.cancelled)
    {
        BinaryResourceLoadResult result;
        result.error = std::move(read.error);
        result.cancelled = read.cancelled;
        return result;
    }
    const auto decoded = talas::project::decodePartialTrackAsset(
        read.bytes,
        shouldCancel);
    if (!decoded.ok())
    {
        BinaryResourceLoadResult result;
        result.error = decoded.error->message;
        result.cancelled = decoded.error->code
            == talas::project::PartialTrackAssetErrorCode::Cancelled;
        return result;
    }
    auto contentHash = talas::project::calculateProjectAssetSha256(
        read.bytes,
        shouldCancel);
    if (!contentHash.ok())
    {
        BinaryResourceLoadResult result;
        result.error = contentHash.error->message;
        result.cancelled = contentHash.error->code
            == talas::project::ProjectAssetInspectionErrorCode::Cancelled;
        return result;
    }
    return {
        std::move(read.bytes),
        {},
        std::move(contentHash.contentHash),
    };
}
}
