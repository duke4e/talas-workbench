#pragma once

#include "talas/audio/ProjectPackager.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
enum class SampleMapImportDisposition
{
    Reference,
    Embed,
};

enum class SampleMapImportErrorCode
{
    InvalidInput,
    InspectionFailed,
    Cancelled,
    CopyFailed,
    CleanupFailed,
};

struct SampleMapImportError
{
    SampleMapImportErrorCode code =
        SampleMapImportErrorCode::InspectionFailed;
    std::string message;
    std::string path;
};

struct PreparedSampleMapImport
{
    std::vector<talas::project::ProjectAsset> assets;
    std::vector<talas::project::ProjectAsset> expectedAssets;
    std::vector<talas::project::ProjectSampleMapAutoMapCandidate>
        autoMapCandidates;
    std::vector<talas::project::ProjectSampleRegion> regions;
    std::vector<std::filesystem::path> createdFiles;
    std::vector<ProjectPackageImportResult> cleanupImports;
};

struct SampleMapImportOptions
{
    std::vector<std::filesystem::path> sourcePaths;
    std::optional<std::filesystem::path> projectFilePath;
    SampleMapImportDisposition disposition =
        SampleMapImportDisposition::Reference;
    std::uint8_t lowNote = 0;
    std::uint8_t highNote = 127;
    std::uint8_t anchorRootNote = 60;
    // Retain inspected candidates when a caller will present range controls.
    bool deferAutoMapRangeValidation = false;
    std::function<bool()> shouldCancel;
    std::function<void(double)> reportProgress;
};

struct SampleMapImportResult
{
    PreparedSampleMapImport candidate;
    std::optional<SampleMapImportError> error;

    [[nodiscard]] bool ok() const
    {
        return !error.has_value();
    }

    [[nodiscard]] bool cancelled() const
    {
        return error.has_value()
            && error->code == SampleMapImportErrorCode::Cancelled;
    }
};

// Performs filesystem inspection/copy only and returns inert project records.
// The caller publishes candidate assets/regions through ProjectEditor, then
// either retains createdFiles or discards them with the function below.
[[nodiscard]] SampleMapImportResult prepareSampleMapImport(
    const talas::project::ProjectDocument& document,
    const SampleMapImportOptions& options);

// Removes only files that this uncommitted candidate created. Successfully
// removed paths are erased; failures remain for an explicit retry.
[[nodiscard]] std::string discardPreparedSampleMapImportFiles(
    PreparedSampleMapImport& candidate);
}
