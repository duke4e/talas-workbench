#pragma once

#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <optional>
#include <string>

namespace talas::project
{
enum class UnsavedChangesChoice
{
    Save,
    Discard,
    Cancel
};

[[nodiscard]] UnsavedChangesChoice unsavedChangesChoiceForButtonIndex(int buttonIndex) noexcept;

class ProjectEditBoundary
{
public:
    explicit ProjectEditBoundary(
        const ProjectDocument& document,
        std::optional<std::filesystem::path> projectPath = {});

    [[nodiscard]] bool isDirty(const ProjectDocument& document) const;
    [[nodiscard]] const std::optional<std::filesystem::path>& projectPath() const noexcept;
    [[nodiscard]] const ProjectDocument& cleanDocument() const noexcept;

    void markSaved(const ProjectDocument& document, std::filesystem::path projectPath);
    void markLoaded(const ProjectDocument& document, std::filesystem::path projectPath);
    void markNew(const ProjectDocument& document);
    void markRecovered(
        const ProjectDocument& cleanDocument,
        std::optional<std::filesystem::path> projectPath);

private:
    void establishCleanBoundary(const ProjectDocument& document);

    ProjectDocument cleanDocument_;
    std::string cleanSnapshot_;
    std::optional<std::filesystem::path> projectPath_;
};
}
