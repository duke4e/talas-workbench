#include "talas/project/PartialTrackAsset.h"

#include "talas/project/SosSpectrumFile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::array<std::uint8_t, 8> magic{
    'K', 'C', 'P', 'A', 'R', 'T', '1', 0};
constexpr std::uint32_t headerBytes = 48;
constexpr std::size_t frameRecordBytes = 24;
constexpr std::size_t pointRecordBytes = 32;

[[nodiscard]] PartialTrackAssetError error(
    PartialTrackAssetErrorCode code,
    std::string message,
    std::size_t frameIndex = 0,
    std::size_t pointIndex = 0)
{
    return {code, frameIndex, pointIndex, std::move(message)};
}

[[nodiscard]] bool checkedAdd(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool checkedMultiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::size_t>::max() / left)
    {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] std::optional<std::size_t> encodedSize(
    std::size_t frameCount,
    std::size_t pointCount) noexcept
{
    std::size_t frameSize = 0;
    std::size_t pointSize = 0;
    std::size_t result = headerBytes;
    if (!checkedMultiply(frameCount, frameRecordBytes, frameSize)
        || !checkedMultiply(pointCount, pointRecordBytes, pointSize)
        || !checkedAdd(result, frameSize, result)
        || !checkedAdd(result, pointSize, result)
        || result > maximumPartialTrackAssetBytes)
    {
        return std::nullopt;
    }
    return result;
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (unsigned shift = 0; shift < 64; shift += 8)
        output.push_back(static_cast<std::uint8_t>(value >> shift));
}

[[nodiscard]] bool readU32(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset,
    std::uint32_t& value) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < 4)
        return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return true;
}

[[nodiscard]] bool readU64(
    std::span<const std::uint8_t> bytes,
    std::size_t& offset,
    std::uint64_t& value) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < 8)
        return false;
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

[[nodiscard]] bool canonicalNonNegative(double value) noexcept
{
    return std::isfinite(value)
        && value >= 0.0
        && !(value == 0.0 && std::signbit(value));
}
}

std::optional<PartialTrackAssetError> validatePartialTrackAsset(
    const PartialTrackAsset& asset)
{
    if (!std::isfinite(asset.sampleRate)
        || asset.sampleRate <= 0.0
        || asset.sampleRate > maximumPartialTrackSampleRate)
    {
        return error(
            PartialTrackAssetErrorCode::InvalidSampleRate,
            "Partial-track sample rate must be finite and in (0, 768000]");
    }
    if (asset.sourceFrameCount == 0
        || asset.sourceFrameCount > maximumPartialTrackSourceFrames)
    {
        return error(
            PartialTrackAssetErrorCode::InvalidSourceExtent,
            "Partial-track source frame extent is outside the exact supported range");
    }
    if (asset.frames.empty()
        || asset.frames.size() > maximumPartialTrackAssetFrames)
    {
        return error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track frame count is outside the supported range");
    }

    std::size_t totalPoints = 0;
    std::unordered_set<std::uint64_t> trackIds;
    try
    {
        trackIds.reserve(std::min(
            maximumPartialTrackAssetTracks,
            asset.frames.size() * 2));
        std::uint64_t previousSegment = 0;
        std::uint64_t previousSourceFrame = 0;
        for (std::size_t frameIndex = 0;
             frameIndex < asset.frames.size();
             ++frameIndex)
        {
            const auto& frame = asset.frames[frameIndex];
            if (frame.segmentId == 0
                || (frameIndex == 0 && frame.segmentId != 1)
                || (frameIndex != 0
                    && (frame.segmentId < previousSegment
                        || frame.segmentId > previousSegment + 1)))
            {
                return error(
                    PartialTrackAssetErrorCode::InvalidSegment,
                    "Partial-track segments must start at one and advance without gaps",
                    frameIndex);
            }
            if (frame.sourceFrame >= asset.sourceFrameCount
                || (frameIndex != 0
                    && frame.sourceFrame <= previousSourceFrame))
            {
                return error(
                    PartialTrackAssetErrorCode::InvalidFrameOrder,
                    "Partial-track source frames must be strictly increasing and inside the source extent",
                    frameIndex);
            }
            if (frame.points.size() > maximumPartialTrackPointsPerFrame
                || frame.points.size()
                    > maximumPartialTrackAssetPoints - totalPoints)
            {
                return error(
                    PartialTrackAssetErrorCode::LimitExceeded,
                    "Partial-track point count exceeds the fixed asset limits",
                    frameIndex);
            }
            totalPoints += frame.points.size();
            std::uint64_t previousTrackId = 0;
            for (std::size_t pointIndex = 0;
                 pointIndex < frame.points.size();
                 ++pointIndex)
            {
                const auto& point = frame.points[pointIndex];
                if (point.trackId == 0
                    || (pointIndex != 0
                        && point.trackId <= previousTrackId))
                {
                    return error(
                        PartialTrackAssetErrorCode::InvalidTrackIdentity,
                        "Partial-track frame identities must be nonzero, unique, and ascending",
                        frameIndex,
                        pointIndex);
                }
                if (!canonicalNonNegative(point.amplitude)
                    || !std::isfinite(point.frequencyHz)
                    || point.frequencyHz <= 0.0
                    || point.frequencyHz > asset.sampleRate * 0.5
                    || !canonicalNonNegative(point.bandwidthHz)
                    || point.bandwidthHz > asset.sampleRate * 0.5)
                {
                    return error(
                        PartialTrackAssetErrorCode::InvalidPointValue,
                        "Partial-track amplitude/frequency/bandwidth values are invalid",
                        frameIndex,
                        pointIndex);
                }
                trackIds.insert(point.trackId);
                if (trackIds.size() > maximumPartialTrackAssetTracks)
                {
                    return error(
                        PartialTrackAssetErrorCode::LimitExceeded,
                        "Partial-track asset exceeds the fixed stable-track limit",
                        frameIndex,
                        pointIndex);
                }
                previousTrackId = point.trackId;
            }
            previousSegment = frame.segmentId;
            previousSourceFrame = frame.sourceFrame;
        }
    }
    catch (const std::bad_alloc&)
    {
        return error(
            PartialTrackAssetErrorCode::AllocationFailed,
            "Partial-track validation allocation failed");
    }

    if (!encodedSize(asset.frames.size(), totalPoints).has_value())
    {
        return error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track encoded bytes exceed the fixed asset limit");
    }
    // The markers within the source, the loop markers both
    // there and in order.
    if (asset.markers.size() > maximumPartialTrackAssetMarkers
        || std::ranges::any_of(asset.markers, [&](const PartialTrackAssetMarker& marker) {
               return marker.label.size() > maximumPartialTrackMarkerLabelBytes
                   || marker.sourceFrame > asset.sourceFrameCount;
           }))
    {
        return error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track markers exceed the fixed limits or lie outside the source");
    }
    if (asset.loopStartFrame.has_value() != asset.loopEndFrame.has_value()
        || (asset.loopStartFrame.has_value()
            && (*asset.loopStartFrame >= *asset.loopEndFrame
                || *asset.loopEndFrame > asset.sourceFrameCount)))
    {
        return error(
            PartialTrackAssetErrorCode::InvalidSourceExtent,
            "Partial-track loop markers must both lie in the source, the start before the end");
    }
    return std::nullopt;
}

namespace
{
[[nodiscard]] bool marked(const PartialTrackAsset& asset) noexcept
{
    return !asset.markers.empty() || asset.loopStartFrame.has_value();
}

// The markers after the frames: their count, whether there is a loop, its
// start and end, then each marker's frame, label length and label.
[[nodiscard]] std::size_t markerTrailerBytes(const PartialTrackAsset& asset) noexcept
{
    std::size_t bytes = 4 + 4 + 8 + 8;
    for (const auto& marker : asset.markers)
        bytes += 8 + 4 + 4 + marker.label.size();
    return bytes;
}
}

PartialTrackAssetEncodeResult encodePartialTrackAsset(
    const PartialTrackAsset& asset)
{
    PartialTrackAssetEncodeResult result;
    result.error = validatePartialTrackAsset(asset);
    if (result.error.has_value())
        return result;

    std::size_t pointCount = 0;
    for (const auto& frame : asset.frames)
        pointCount += frame.points.size();
    auto size = encodedSize(asset.frames.size(), pointCount);
    if (size.has_value() && marked(asset))
    {
        *size += markerTrailerBytes(asset);
        if (*size > maximumPartialTrackAssetBytes)
            size.reset();
    }
    if (!size.has_value())
    {
        result.error = error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track encoded bytes exceed the fixed asset limit");
        return result;
    }
    try
    {
        result.bytes.reserve(*size);
        result.bytes.insert(result.bytes.end(), magic.begin(), magic.end());
        appendU32(result.bytes,
            marked(asset) ? partialTrackAssetMarkedFormatVersion : partialTrackAssetFormatVersion);
        appendU32(result.bytes, headerBytes);
        appendU64(result.bytes, asset.frames.size());
        appendU64(result.bytes, pointCount);
        appendU64(result.bytes, asset.sourceFrameCount);
        appendU64(result.bytes, std::bit_cast<std::uint64_t>(asset.sampleRate));
        for (const auto& frame : asset.frames)
        {
            appendU64(result.bytes, frame.segmentId);
            appendU64(result.bytes, frame.sourceFrame);
            appendU32(
                result.bytes,
                static_cast<std::uint32_t>(frame.points.size()));
            appendU32(result.bytes, 0);
            for (const auto& point : frame.points)
            {
                appendU64(result.bytes, point.trackId);
                appendU64(
                    result.bytes,
                    std::bit_cast<std::uint64_t>(point.amplitude));
                appendU64(
                    result.bytes,
                    std::bit_cast<std::uint64_t>(point.frequencyHz));
                appendU64(
                    result.bytes,
                    std::bit_cast<std::uint64_t>(point.bandwidthHz));
            }
        }
        if (marked(asset))
        {
            appendU32(result.bytes, static_cast<std::uint32_t>(asset.markers.size()));
            appendU32(result.bytes, asset.loopStartFrame.has_value() ? 1U : 0U);
            appendU64(result.bytes, asset.loopStartFrame.value_or(0));
            appendU64(result.bytes, asset.loopEndFrame.value_or(0));
            for (const auto& marker : asset.markers)
            {
                appendU64(result.bytes, marker.sourceFrame);
                appendU32(result.bytes, static_cast<std::uint32_t>(marker.label.size()));
                appendU32(result.bytes, 0);
                result.bytes.insert(result.bytes.end(), marker.label.begin(), marker.label.end());
            }
        }
    }
    catch (const std::bad_alloc&)
    {
        result.bytes.clear();
        result.error = error(
            PartialTrackAssetErrorCode::AllocationFailed,
            "Partial-track encoding allocation failed");
        return result;
    }
    if (result.bytes.size() != *size)
    {
        result.bytes.clear();
        result.error = error(
            PartialTrackAssetErrorCode::SizeMismatch,
            "Partial-track encoder produced an unexpected byte count");
    }
    return result;
}

PartialTrackAssetDecodeResult decodePartialTrackAsset(
    std::span<const std::uint8_t> bytes,
    std::function<bool()> shouldCancel)
{
    // A Sum-of-Sines spectrum file (.spc) is read as a .talaspartial
    // is, wherever a spectrum is taken.
    if (isSosSpectrumFile(bytes))
        return decodeSosSpectrumFile(bytes, std::move(shouldCancel));
    PartialTrackAssetDecodeResult result;
    if (bytes.size() < headerBytes
        || !std::equal(magic.begin(), magic.end(), bytes.begin()))
    {
        result.error = error(
            PartialTrackAssetErrorCode::InvalidHeader,
            "Partial-track asset magic/header is invalid");
        return result;
    }
    if (bytes.size() > maximumPartialTrackAssetBytes)
    {
        result.error = error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track asset exceeds the fixed byte limit");
        return result;
    }

    std::size_t offset = magic.size();
    std::uint32_t version = 0;
    std::uint32_t declaredHeaderBytes = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t pointCount = 0;
    std::uint64_t sourceFrameCount = 0;
    std::uint64_t sampleRateBits = 0;
    if (!readU32(bytes, offset, version)
        || !readU32(bytes, offset, declaredHeaderBytes)
        || !readU64(bytes, offset, frameCount)
        || !readU64(bytes, offset, pointCount)
        || !readU64(bytes, offset, sourceFrameCount)
        || !readU64(bytes, offset, sampleRateBits))
    {
        result.error = error(
            PartialTrackAssetErrorCode::InvalidHeader,
            "Partial-track asset header is truncated");
        return result;
    }
    if (version != partialTrackAssetFormatVersion && version != partialTrackAssetMarkedFormatVersion)
    {
        result.error = error(
            PartialTrackAssetErrorCode::UnsupportedVersion,
            "Partial-track asset version is unsupported");
        return result;
    }
    if (declaredHeaderBytes != headerBytes
        || frameCount == 0
        || frameCount > maximumPartialTrackAssetFrames
        || pointCount > maximumPartialTrackAssetPoints
        || frameCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())
        || pointCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        result.error = error(
            PartialTrackAssetErrorCode::LimitExceeded,
            "Partial-track header declares unsupported counts");
        return result;
    }
    const auto expectedSize = encodedSize(
        static_cast<std::size_t>(frameCount),
        static_cast<std::size_t>(pointCount));
    if (!expectedSize.has_value()
        || (version == partialTrackAssetFormatVersion ? *expectedSize != bytes.size() : *expectedSize >= bytes.size()))
    {
        result.error = error(
            PartialTrackAssetErrorCode::SizeMismatch,
            "Partial-track asset byte count does not match its header");
        return result;
    }

    try
    {
        result.asset.sampleRate = std::bit_cast<double>(sampleRateBits);
        result.asset.sourceFrameCount = sourceFrameCount;
        result.asset.frames.reserve(static_cast<std::size_t>(frameCount));
        std::size_t decodedPoints = 0;
        for (std::size_t frameIndex = 0;
             frameIndex < static_cast<std::size_t>(frameCount);
             ++frameIndex)
        {
            if (shouldCancel && shouldCancel())
            {
                result.error = error(
                    PartialTrackAssetErrorCode::Cancelled,
                    "Partial-track decoding cancelled",
                    frameIndex);
                result.asset = {};
                return result;
            }
            PartialTrackFrame frame;
            std::uint32_t framePointCount = 0;
            std::uint32_t reserved = 0;
            if (!readU64(bytes, offset, frame.segmentId)
                || !readU64(bytes, offset, frame.sourceFrame)
                || !readU32(bytes, offset, framePointCount)
                || !readU32(bytes, offset, reserved)
                || reserved != 0
                || framePointCount > maximumPartialTrackPointsPerFrame
                || framePointCount
                    > static_cast<std::size_t>(pointCount) - decodedPoints)
            {
                result.error = error(
                    PartialTrackAssetErrorCode::InvalidHeader,
                    "Partial-track frame record is malformed",
                    frameIndex);
                result.asset = {};
                return result;
            }
            frame.points.reserve(framePointCount);
            for (std::size_t pointIndex = 0;
                 pointIndex < framePointCount;
                 ++pointIndex)
            {
                if ((pointIndex & 4095U) == 0U
                    && shouldCancel
                    && shouldCancel())
                {
                    result.error = error(
                        PartialTrackAssetErrorCode::Cancelled,
                        "Partial-track decoding cancelled",
                        frameIndex,
                        pointIndex);
                    result.asset = {};
                    return result;
                }
                PartialTrackPoint point;
                std::uint64_t amplitudeBits = 0;
                std::uint64_t frequencyBits = 0;
                std::uint64_t bandwidthBits = 0;
                if (!readU64(bytes, offset, point.trackId)
                    || !readU64(bytes, offset, amplitudeBits)
                    || !readU64(bytes, offset, frequencyBits)
                    || !readU64(bytes, offset, bandwidthBits))
                {
                    result.error = error(
                        PartialTrackAssetErrorCode::InvalidHeader,
                        "Partial-track point record is truncated",
                        frameIndex,
                        pointIndex);
                    result.asset = {};
                    return result;
                }
                point.amplitude = std::bit_cast<double>(amplitudeBits);
                point.frequencyHz = std::bit_cast<double>(frequencyBits);
                point.bandwidthHz = std::bit_cast<double>(bandwidthBits);
                frame.points.push_back(point);
            }
            decodedPoints += framePointCount;
            result.asset.frames.push_back(std::move(frame));
        }
        // A version 2 file's markers follow its frames.
        if (version == partialTrackAssetMarkedFormatVersion)
        {
            std::uint32_t markerCount = 0;
            std::uint32_t loop = 0;
            std::uint64_t loopStart = 0;
            std::uint64_t loopEnd = 0;
            if (!readU32(bytes, offset, markerCount) || !readU32(bytes, offset, loop)
                || !readU64(bytes, offset, loopStart) || !readU64(bytes, offset, loopEnd) || loop > 1
                || markerCount > maximumPartialTrackAssetMarkers)
            {
                result.error = error(
                    PartialTrackAssetErrorCode::InvalidHeader,
                    "Partial-track marker header is malformed");
                result.asset = {};
                return result;
            }
            if (loop == 1)
            {
                result.asset.loopStartFrame = loopStart;
                result.asset.loopEndFrame = loopEnd;
            }
            for (std::uint32_t index = 0; index < markerCount; ++index)
            {
                PartialTrackAssetMarker marker;
                std::uint32_t labelBytes = 0;
                std::uint32_t reserved = 0;
                if (!readU64(bytes, offset, marker.sourceFrame) || !readU32(bytes, offset, labelBytes)
                    || !readU32(bytes, offset, reserved) || reserved != 0
                    || labelBytes > maximumPartialTrackMarkerLabelBytes || labelBytes > bytes.size() - offset)
                {
                    result.error = error(
                        PartialTrackAssetErrorCode::InvalidHeader,
                        "Partial-track marker record is malformed");
                    result.asset = {};
                    return result;
                }
                marker.label.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + labelBytes));
                offset += labelBytes;
                result.asset.markers.push_back(std::move(marker));
            }
        }
        if (decodedPoints != pointCount || offset != bytes.size())
        {
            result.error = error(
                PartialTrackAssetErrorCode::SizeMismatch,
                "Partial-track decoded counts do not match the header");
            result.asset = {};
            return result;
        }
    }
    catch (const std::bad_alloc&)
    {
        result.asset = {};
        result.error = error(
            PartialTrackAssetErrorCode::AllocationFailed,
            "Partial-track decoding allocation failed");
        return result;
    }

    result.error = validatePartialTrackAsset(result.asset);
    if (result.error.has_value())
        result.asset = {};
    return result;
}
}
