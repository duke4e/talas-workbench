#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
inline constexpr std::uint32_t partialTrackAssetFormatVersion = 1;
// A spectrum file holding markers in its header:
// version 2; one without them is still written as version 1.
inline constexpr std::uint32_t partialTrackAssetMarkedFormatVersion = 2;
inline constexpr std::size_t maximumPartialTrackAssetMarkers = 1024;
inline constexpr std::size_t maximumPartialTrackMarkerLabelBytes = 255;
inline constexpr int partialTrackProjectFormatVersion = 22;
inline constexpr std::size_t maximumPartialTrackAssetFrames = 1U << 20U;
inline constexpr std::size_t maximumPartialTrackPointsPerFrame = 4096;
inline constexpr std::size_t maximumPartialTrackAssetPoints = 1U << 22U;
inline constexpr std::size_t maximumPartialTrackAssetTracks = 1U << 16U;
inline constexpr std::size_t maximumPartialTrackAssetBytes =
    256U * 1024U * 1024U;
inline constexpr std::uint64_t maximumPartialTrackSourceFrames =
    9007199254740991ULL;
inline constexpr double maximumPartialTrackSampleRate = 768000.0;
inline constexpr std::string_view partialTrackAssetEncoding =
    "talas.partial-tracks-v1";

struct PartialTrackPoint
{
    std::uint64_t trackId = 0;
    double amplitude = 0.0;
    double frequencyHz = 0.0;
    double bandwidthHz = 0.0;

    bool operator==(const PartialTrackPoint&) const = default;
};

struct PartialTrackFrame
{
    std::uint64_t segmentId = 0;
    std::uint64_t sourceFrame = 0;
    std::vector<PartialTrackPoint> points;

    bool operator==(const PartialTrackFrame&) const = default;
};

// A marker of a spectrum file: its label and the source frame it marks
//.
struct PartialTrackAssetMarker
{
    std::string label;
    std::uint64_t sourceFrame = 0;

    bool operator==(const PartialTrackAssetMarker&) const = default;
};

struct PartialTrackAsset
{
    double sampleRate = 0.0;
    std::uint64_t sourceFrameCount = 0;
    std::vector<PartialTrackFrame> frames;
    // The markers and the start and end loop
    // markers, kept in the file's header; the resynthesis loops between the
    // loop markers.
    std::vector<PartialTrackAssetMarker> markers;
    std::optional<std::uint64_t> loopStartFrame;
    std::optional<std::uint64_t> loopEndFrame;

    bool operator==(const PartialTrackAsset&) const = default;
};

enum class PartialTrackAssetErrorCode : std::uint8_t
{
    InvalidHeader,
    UnsupportedVersion,
    SizeMismatch,
    LimitExceeded,
    InvalidSampleRate,
    InvalidSourceExtent,
    InvalidFrameOrder,
    InvalidSegment,
    InvalidTrackIdentity,
    InvalidPointValue,
    Cancelled,
    AllocationFailed,
};

struct PartialTrackAssetError
{
    PartialTrackAssetErrorCode code =
        PartialTrackAssetErrorCode::InvalidHeader;
    std::size_t frameIndex = 0;
    std::size_t pointIndex = 0;
    std::string message;
};

struct PartialTrackAssetEncodeResult
{
    std::vector<std::uint8_t> bytes;
    std::optional<PartialTrackAssetError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

struct PartialTrackAssetDecodeResult
{
    PartialTrackAsset asset;
    std::optional<PartialTrackAssetError> error;

    [[nodiscard]] bool ok() const noexcept
    {
        return !error.has_value();
    }
};

[[nodiscard]] std::optional<PartialTrackAssetError>
validatePartialTrackAsset(const PartialTrackAsset& asset);

[[nodiscard]] PartialTrackAssetEncodeResult encodePartialTrackAsset(
    const PartialTrackAsset& asset);

// A .talaspartial, or an .spc spectrum file (talas/project/SosSpectrumFile.h).
[[nodiscard]] PartialTrackAssetDecodeResult decodePartialTrackAsset(
    std::span<const std::uint8_t> bytes,
    std::function<bool()> shouldCancel = {});
}
