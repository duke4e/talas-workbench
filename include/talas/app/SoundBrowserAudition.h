#pragma once

#include "talas/app/SoundBrowserModel.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// What the Sound Browser plays and keeps: a
// Sound of a Sound file, its first Audio input fed by the replacement input
// while it is connected (an audio input channel, a sample, or another Sound)
// and by the default sine otherwise; and that combination added to the
// project as a Sound of its own.
namespace talas::app
{
struct SoundBrowserAuditionDocument
{
    std::optional<talas::project::ProjectDocument> document;
    std::size_t outputChannelCount = 0;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return document.has_value(); }
};

// inputSoundFile is the Sound file of a Sound designated as the input.
[[nodiscard]] SoundBrowserAuditionDocument soundBrowserAuditionDocument(
    const talas::project::ProjectDocument& soundFile,
    std::string_view definitionId,
    const SoundBrowserReplacementInput& input,
    const std::optional<talas::project::ProjectDocument>& inputSoundFile,
    const talas::core::NodeRegistry& registry);

struct SoundBrowserImport
{
    std::optional<talas::project::ProjectDocument> document;
    std::string rootDefinitionId;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return document.has_value(); }
};

// The project with the Sound dragged in from the browser: the Sound
// itself, or with the replacement input connected a new Sound named for both
// that holds the Sound fed by that input.
[[nodiscard]] SoundBrowserImport soundBrowserImport(
    const talas::project::ProjectDocument& project,
    const talas::project::ProjectDocument& soundFile,
    std::string_view definitionId,
    const SoundBrowserReplacementInput& input,
    const std::optional<talas::project::ProjectDocument>& inputSoundFile,
    const talas::core::NodeRegistry& registry);
}
