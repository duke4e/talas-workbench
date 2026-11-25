#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/Runtime.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::audio
{
enum class ProjectWavExportOutcome
{
    Succeeded,
    Cancelled,
    Failed
};

struct ProjectWavExportOptions
{
    std::filesystem::path outputPath;
    std::optional<std::filesystem::path> projectFilePath;
    double sampleRate = 48000.0;
    double durationSeconds = 10.0;
    std::size_t maxBlockFrames = 4096;
    std::function<bool()> shouldCancel;
    std::function<void(double)> reportProgress;
    std::function<bool()> admitPublication;
};

struct ProjectWavExportResult
{
    ProjectWavExportOutcome outcome = ProjectWavExportOutcome::Failed;
    std::size_t frameCount = 0;
    std::size_t channelCount = 0;
    std::vector<talas::runtime::Diagnostic> diagnostics;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return outcome == ProjectWavExportOutcome::Succeeded;
    }

    [[nodiscard]] bool cancelled() const
    {
        return outcome == ProjectWavExportOutcome::Cancelled;
    }
};

[[nodiscard]] ProjectWavExportResult exportProjectToPcm16Wav(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    ProjectWavExportOptions options);
}
