#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace talas::project
{
inline constexpr std::string_view diskCacheCaptureProvenancePrefix =
    "talas.diskCache.capture.v1:";
inline constexpr std::string_view diskCacheCaptureProvenanceNamespace =
    "talas.diskCache.capture.";
inline constexpr std::size_t diskCacheCaptureFingerprintBytes = 64;
inline constexpr std::uint64_t maximumDiskCacheCaptureFrames = 16'777'216;
inline constexpr std::uint32_t minimumDiskCacheCaptureSampleRate = 8'000;
inline constexpr std::uint32_t maximumDiskCacheCaptureSampleRate = 192'000;

enum class DiskCacheAssetAuthority
{
    LegacyPassthrough,
    CapturedPlayback,
    InvalidCapturedAsset,
};

enum class DiskCacheCaptureRangeError
{
    None,
    Disabled,
    InvalidSampleRate,
    InvalidLoop,
    Overflow,
    ExceedsFrameLimit,
};

enum class DiskCacheCaptureGenerationError
{
    None,
    InvalidAssetId,
    UnrelatedAsset,
    Exhausted,
};

struct DiskCacheCaptureGenerationResult
{
    std::optional<std::uint64_t> generation;
    DiskCacheCaptureGenerationError error =
        DiskCacheCaptureGenerationError::None;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return generation.has_value()
            && error == DiskCacheCaptureGenerationError::None;
    }
};

struct DiskCacheCaptureRange
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;

    [[nodiscard]] constexpr std::uint64_t frameCount() const noexcept
    {
        return endFrame - startFrame;
    }

    bool operator==(const DiskCacheCaptureRange&) const = default;
};

struct DiskCacheCaptureRangeResult
{
    std::optional<DiskCacheCaptureRange> range;
    DiskCacheCaptureRangeError error = DiskCacheCaptureRangeError::None;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return range.has_value()
            && error == DiskCacheCaptureRangeError::None;
    }
};

[[nodiscard]] bool isCanonicalDiskCacheCaptureFingerprint(
    std::string_view fingerprint) noexcept;

[[nodiscard]] std::string makeDiskCacheCaptureProvenance(
    std::string_view fingerprint);

[[nodiscard]] std::optional<std::string_view> diskCacheCaptureFingerprint(
    std::string_view provenance) noexcept;

[[nodiscard]] DiskCacheAssetAuthority diskCacheAssetAuthority(
    const ProjectAsset& asset) noexcept;

[[nodiscard]] bool isValidDiskCacheAssetPath(
    std::string_view path) noexcept;

[[nodiscard]] DiskCacheCaptureRangeResult diskCacheCaptureRange(
    const TimelineLoopRegion& loop,
    double sampleRate) noexcept;

// Returns the only generation that a new capture may publish for this exact
// asset identity. Existing non-capture authority and the final exactly
// representable project generation fail before render or filesystem work.
[[nodiscard]] DiskCacheCaptureGenerationResult
nextDiskCacheCaptureAssetGeneration(
    const ProjectDocument& document,
    std::string_view assetId) noexcept;

// Hashes the exact expanded upstream execution closure for one DiskCache
// structural source path. The cache node itself is excluded so committing its
// fileName/record result cannot stale the capture it just produced.
[[nodiscard]] std::string projectDiskCacheSourceFingerprint(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::string_view sourcePath,
    std::uint32_t sampleRate,
    std::string& error);
}
