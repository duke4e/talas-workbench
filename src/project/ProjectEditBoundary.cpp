#include "talas/project/ProjectEditBoundary.h"

#include "talas/project/ProjectJson.h"

#include <utility>

namespace talas::project
{
UnsavedChangesChoice unsavedChangesChoiceForButtonIndex(int buttonIndex) noexcept
{
    switch (buttonIndex)
    {
    case 0: return UnsavedChangesChoice::Save;
    case 1: return UnsavedChangesChoice::Discard;
    case 2: return UnsavedChangesChoice::Cancel;
    default: return UnsavedChangesChoice::Cancel;
    }
}

ProjectEditBoundary::ProjectEditBoundary(
    const ProjectDocument& document,
    std::optional<std::filesystem::path> projectPath)
    : projectPath_(std::move(projectPath))
{
    establishCleanBoundary(document);
}

bool ProjectEditBoundary::isDirty(const ProjectDocument& document) const
{
    return serializeProjectDocument(document) != cleanSnapshot_;
}

const std::optional<std::filesystem::path>& ProjectEditBoundary::projectPath() const noexcept
{
    return projectPath_;
}

const ProjectDocument& ProjectEditBoundary::cleanDocument() const noexcept
{
    return cleanDocument_;
}

void ProjectEditBoundary::markSaved(
    const ProjectDocument& document,
    std::filesystem::path projectPath)
{
    establishCleanBoundary(document);
    projectPath_ = std::move(projectPath);
}

void ProjectEditBoundary::markLoaded(
    const ProjectDocument& document,
    std::filesystem::path projectPath)
{
    markSaved(document, std::move(projectPath));
}

void ProjectEditBoundary::markNew(const ProjectDocument& document)
{
    establishCleanBoundary(document);
    projectPath_.reset();
}

void ProjectEditBoundary::markRecovered(
    const ProjectDocument& cleanDocument,
    std::optional<std::filesystem::path> projectPath)
{
    establishCleanBoundary(cleanDocument);
    projectPath_ = std::move(projectPath);
}

void ProjectEditBoundary::establishCleanBoundary(const ProjectDocument& document)
{
    cleanDocument_ = document;
    cleanSnapshot_ = serializeProjectDocument(document);
}
}
