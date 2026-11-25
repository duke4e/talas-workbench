#pragma once

#include "talas/audio/AudioFilePublication.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectEditor.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::audio
{
enum class DiskCacheCaptureOutcome
{
    Prepared,
    Cancelled,
    Failed,
};

struct DiskCacheCaptureOptions
{
    std::filesystem::path projectFilePath;
    std::string sourcePath;
    std::string assetId;
    std::string relativeAssetPath;
    std::uint64_t assetGeneration = 1;
    std::uint64_t expectedDocumentAuthorityGeneration = 0;
    std::string expectedCanonicalDocumentBytes;
    std::uint64_t expectedDeviceGeneration = 0;
    double sampleRate = 0.0;
    std::size_t maximumBlockFrames = 4096;
    std::function<bool()> shouldCancel;
    std::function<void(double)> reportProgress;
};

class PreparedDiskCacheCapture
{
public:
    PreparedDiskCacheCapture(
        talas::project::DiskCacheCapturePublication editorPublication,
        std::unique_ptr<AudioFilePublication> filePublication,
        std::uint64_t expectedDeviceGeneration);

    PreparedDiskCacheCapture(const PreparedDiskCacheCapture&) = delete;
    PreparedDiskCacheCapture& operator=(
        const PreparedDiskCacheCapture&) = delete;
    PreparedDiskCacheCapture(PreparedDiskCacheCapture&&) noexcept = default;
    PreparedDiskCacheCapture& operator=(
        PreparedDiskCacheCapture&&) noexcept = default;

    [[nodiscard]] const talas::project::DiskCacheCapturePublication&
    editorPublication() const noexcept;
    [[nodiscard]] talas::project::DiskCacheCapturePublication
    takeEditorPublication() noexcept;
    [[nodiscard]] const std::filesystem::path& destination() const noexcept;
    [[nodiscard]] const std::filesystem::path& temporaryPath() const noexcept;
    [[nodiscard]] std::uint64_t expectedDeviceGeneration() const noexcept;
    [[nodiscard]] bool publishFile(std::string& error) noexcept;
    // Removes only this capture's just-published, byte-exact generation when
    // the paired project/document publication cannot complete.
    [[nodiscard]] bool retractPublishedFile(std::string& error) noexcept;

private:
    talas::project::DiskCacheCapturePublication editorPublication_;
    talas::project::ProjectAsset publishedAssetAuthority_;
    std::unique_ptr<AudioFilePublication> filePublication_;
    std::uint64_t expectedDeviceGeneration_ = 0;
    bool published_ = false;
    bool retracted_ = false;
};

struct DiskCacheCaptureResult
{
    DiskCacheCaptureOutcome outcome = DiskCacheCaptureOutcome::Failed;
    std::unique_ptr<PreparedDiskCacheCapture> prepared;
    std::uint64_t renderedFrames = 0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return outcome == DiskCacheCaptureOutcome::Prepared
            && prepared != nullptr;
    }

    [[nodiscard]] bool cancelled() const noexcept
    {
        return outcome == DiskCacheCaptureOutcome::Cancelled;
    }
};

[[nodiscard]] DiskCacheCaptureResult prepareProjectDiskCacheCapture(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    DiskCacheCaptureOptions options);
}
