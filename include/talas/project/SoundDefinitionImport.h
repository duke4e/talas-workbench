#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumSoundDefinitionImportRemaps = 256;
inline constexpr std::size_t maximumSoundDefinitionImportClosure = 256;
inline constexpr std::size_t maximumSoundDefinitionImportAssets = 256;

struct SoundDefinitionImportIdentityRemap
{
    std::string sourceId;
    std::string destinationId;

    bool operator==(const SoundDefinitionImportIdentityRemap&) const = default;
};

struct SoundDefinitionImportRequest
{
    std::string rootDefinitionId;
    std::vector<SoundDefinitionImportIdentityRemap> definitionRemaps;
    std::vector<SoundDefinitionImportIdentityRemap> assetRemaps;
    // Resolve imported assets in the source context and encode them for the
    // destination context so import cannot silently retarget a relative path.
    std::optional<std::filesystem::path> sourceProjectFilePath;
    std::optional<std::filesystem::path> destinationProjectFilePath;
};

struct SoundDefinitionImportPlan
{
    ProjectDocument document;
    std::string importedRootDefinitionId;
    std::vector<std::string> importedDefinitionIds;
    std::vector<std::string> importedAssetIds;
    bool noOp = false;
};

struct SoundDefinitionImportCollisionPreview
{
    std::optional<SoundDefinitionImportRequest> request;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return request.has_value(); }
};

struct SoundDefinitionImportResult
{
    std::optional<SoundDefinitionImportPlan> plan;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return plan.has_value(); }
};

[[nodiscard]] SoundDefinitionImportResult planSoundDefinitionImport(
    const ProjectDocument& destination,
    const ProjectDocument& source,
    const SoundDefinitionImportRequest& request,
    const talas::core::NodeRegistry& registry);

[[nodiscard]] SoundDefinitionImportCollisionPreview
suggestSoundDefinitionImportCollisionPlan(
    const ProjectDocument& destination,
    const ProjectDocument& source,
    std::string_view rootDefinitionId,
    const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> sourceProjectFilePath = {},
    std::optional<std::filesystem::path> destinationProjectFilePath = {});
}
