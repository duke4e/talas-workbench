#pragma once

#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
inline constexpr std::uint64_t maximumProjectAssetJsonInteger =
    9007199254740991ULL;
inline constexpr std::string_view pcm16WavAssetEncoding =
    "pcm16-wav";
// Any other WAV or AIFF sample file a DiskPlayer streams, its word size and
// byte order read from its header.
inline constexpr std::string_view sampleFileAssetEncoding =
    "sample-file";

enum class ProjectAssetInspectionErrorCode
{
    InvalidManifest,
    Unavailable,
    NotRegularFile,
    ReadFailed,
    UnsupportedMedia,
    InvalidMedia,
    ContentMismatch,
    MediaMismatch,
    Cancelled,
};

using ProjectAssetInspectionCancellationCheck =
    std::function<bool()>;

struct ProjectAssetInspectionError
{
    ProjectAssetInspectionErrorCode code =
        ProjectAssetInspectionErrorCode::ReadFailed;
    std::string message;
    std::string path;
};

struct ProjectAssetHashResult
{
    std::string contentHash;
    std::optional<ProjectAssetInspectionError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

struct ProjectAssetInspectionResult
{
    ProjectAsset asset;
    std::optional<ProjectAssetInspectionError> error;
    // Populated only when the path overload had to acquire a bounded,
    // immutable byte snapshot (currently captured DiskCache Audio). This lets
    // downstream preparation retain the exact inspected object without
    // reopening its pathname.
    std::vector<std::uint8_t> inspectedBytes;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }
};

[[nodiscard]] bool isCanonicalProjectAssetContentHash(
    std::string_view hash) noexcept;

[[nodiscard]] std::string calculateProjectAssetSha256(
    std::span<const std::uint8_t> bytes);

[[nodiscard]] ProjectAssetHashResult calculateProjectAssetSha256(
    std::span<const std::uint8_t> bytes,
    ProjectAssetInspectionCancellationCheck shouldCancel);

[[nodiscard]] std::optional<ProjectAssetInspectionError>
validateProjectAssetManifest(const ProjectAsset& asset);

// Validates the cross-document authority between persisted Score clip render
// artifacts and their exact Project Audio manifests. The error path carries
// the owning clip ID.
[[nodiscard]] std::optional<ProjectAssetInspectionError>
validateProjectScoreRenderArtifacts(const ProjectDocument& document);

[[nodiscard]] ProjectAssetHashResult calculateProjectAssetSha256(
    const std::filesystem::path& path,
    ProjectAssetInspectionCancellationCheck shouldCancel = {});

// Inspects local bytes without changing identity or generation. Empty manifest
// metadata is filled; supplied hash/media metadata is treated as an expectation
// and must match exactly.
[[nodiscard]] ProjectAssetInspectionResult inspectProjectAsset(
    const ProjectAsset& manifest,
    const std::filesystem::path& resolvedPath,
    ProjectAssetInspectionCancellationCheck shouldCancel = {});

// Inspects an immutable caller-owned byte snapshot. The diagnostic path is
// presentation-only and is never reopened.
[[nodiscard]] ProjectAssetInspectionResult inspectProjectAsset(
    const ProjectAsset& manifest,
    std::span<const std::uint8_t> bytes,
    const std::filesystem::path& diagnosticPath,
    ProjectAssetInspectionCancellationCheck shouldCancel = {});
}
