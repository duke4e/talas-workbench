#pragma once

#include "talas/app/SoundLibraryModel.h"
#include "talas/project/WorkspaceSessionState.h"

#include <optional>
#include <string>
#include <vector>

namespace talas::app
{
struct SoundLibraryAuditionInput
{
    std::string portId;
    talas::core::PortType type = talas::core::PortType::Audio;
    talas::project::WorkspaceSoundLibraryAuditionSource source =
        talas::project::WorkspaceSoundLibraryAuditionSource::Default;
    double value = 0.0;
    std::optional<std::string> frozenAssetId;

    bool operator==(const SoundLibraryAuditionInput&) const = default;
};

struct SoundLibraryAuditionPreparation
{
    std::optional<talas::project::ProjectDocument> document;
    std::string stableEntryId;
    std::string sourceLabel;
    std::vector<SoundLibraryAuditionInput> inputs;
    std::size_t outputChannelCount = 0;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept
    {
        return document.has_value();
    }
};

// source is the exact project authority for ProjectDefinition entries and the
// freshly verified example snapshot for OriginalExample entries. The function
// never mutates source and authors only a private audition document.
[[nodiscard]] SoundLibraryAuditionPreparation prepareSoundLibraryAudition(
    const talas::project::ProjectDocument& source,
    const SoundLibraryEntry& entry,
    const talas::core::NodeRegistry& registry,
    const std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution>&
        substitutions);

// Removes local substitutions that no longer belong to the exact selected
// entry after an in-place Sound/interface or asset-authority change.
[[nodiscard]] bool reconcileSoundLibraryAuditionSubstitutions(
    talas::project::WorkspaceSoundLibraryState& state,
    const std::vector<SoundLibraryEntry>& entries);
}
