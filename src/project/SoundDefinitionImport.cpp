#include "talas/project/SoundDefinitionImport.h"

#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
template <typename Item, typename Projection>
[[nodiscard]] const Item* findById(
    const std::vector<Item>& items,
    std::string_view id,
    Projection projection)
{
    const auto found = std::ranges::find(items, id, projection);
    return found == items.end() ? nullptr : &*found;
}

[[nodiscard]] bool buildRemap(
    const std::vector<SoundDefinitionImportIdentityRemap>& requested,
    std::unordered_map<std::string, std::string>& output,
    std::string& diagnostic)
{
    if (requested.size() > maximumSoundDefinitionImportRemaps)
    {
        diagnostic = "Import collision plan exceeds the bounded remap count";
        return false;
    }
    std::unordered_set<std::string> destinations;
    for (const auto& remap : requested)
    {
        if (remap.sourceId.empty() || remap.destinationId.empty()
            || remap.sourceId == remap.destinationId
            || !output.emplace(remap.sourceId, remap.destinationId).second
            || !destinations.insert(remap.destinationId).second)
        {
            diagnostic = "Import collision plan contains an empty or duplicate identity";
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string mappedId(
    const std::unordered_map<std::string, std::string>& remaps,
    std::string_view sourceId)
{
    const auto found = remaps.find(std::string{sourceId});
    return found == remaps.end() ? std::string{sourceId} : found->second;
}

[[nodiscard]] std::string nextImportedIdentity(
    std::string_view sourceId,
    const auto& occupied)
{
    const auto base = std::string{sourceId} + "Imported";
    if (!occupied(base))
        return base;
    for (std::size_t ordinal = 2;; ++ordinal)
    {
        const auto candidate = base + std::to_string(ordinal);
        if (!occupied(candidate))
            return candidate;
    }
}

[[nodiscard]] ProjectSoundDefinition remappedDefinition(
    const ProjectSoundDefinition& source,
    const std::unordered_map<std::string, std::string>& definitionRemaps,
    const std::unordered_map<std::string, std::string>& assetRemaps)
{
    auto imported = source;
    imported.definitionId = mappedId(
        definitionRemaps, source.definitionId);
    for (auto& node : imported.graph.nodes)
    {
        if (node.soundDefinitionId.has_value())
            node.soundDefinitionId = mappedId(
                definitionRemaps, *node.soundDefinitionId);
        for (auto& parameter : node.parameters)
        {
            if (parameter.resource.has_value())
                parameter.resource = mappedId(
                    assetRemaps, *parameter.resource);
        }
    }
    for (auto& parameter : imported.exposedParameters)
    {
        if (parameter.defaultValue.resource.has_value())
        {
            parameter.defaultValue.resource = mappedId(
                assetRemaps, *parameter.defaultValue.resource);
        }
    }
    return imported;
}
}

SoundDefinitionImportResult planSoundDefinitionImport(
    const ProjectDocument& destination,
    const ProjectDocument& source,
    const SoundDefinitionImportRequest& request,
    const talas::core::NodeRegistry& registry)
{
    SoundDefinitionImportResult result;
    if (request.rootDefinitionId.empty())
    {
        result.diagnostic = "Reusable definition import requires one exact root identity";
        return result;
    }
    std::unordered_map<std::string, std::string> definitionRemaps;
    std::unordered_map<std::string, std::string> assetRemaps;
    if (!buildRemap(request.definitionRemaps, definitionRemaps, result.diagnostic)
        || !buildRemap(request.assetRemaps, assetRemaps, result.diagnostic))
    {
        return result;
    }

    std::vector<const ProjectSoundDefinition*> closure;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    const auto collect = [&](auto&& self, std::string_view definitionId) -> bool {
        if (visited.contains(std::string{definitionId}))
            return true;
        if (visiting.size() + visited.size()
            >= maximumSoundDefinitionImportClosure)
        {
            result.diagnostic =
                "Reusable definition import exceeds the bounded dependency closure";
            return false;
        }
        if (!visiting.insert(std::string{definitionId}).second)
        {
            result.diagnostic = "Reusable definition import contains a dependency cycle at "
                + std::string{definitionId};
            return false;
        }
        const auto* definition = findById(
            source.soundDefinitions,
            definitionId,
            &ProjectSoundDefinition::definitionId);
        if (definition == nullptr)
        {
            result.diagnostic = "Reusable definition import is missing dependency "
                + std::string{definitionId};
            return false;
        }
        for (const auto& node : definition->graph.nodes)
        {
            if (node.soundDefinitionId.has_value()
                && !self(self, *node.soundDefinitionId))
            {
                return false;
            }
        }
        visiting.erase(std::string{definitionId});
        visited.insert(std::string{definitionId});
        closure.push_back(definition);
        return true;
    };
    if (!collect(collect, request.rootDefinitionId))
        return result;
    for (const auto& remap : request.definitionRemaps)
    {
        if (!visited.contains(remap.sourceId))
        {
            result.diagnostic =
                "Definition collision plan references content outside the import closure: "
                + remap.sourceId;
            return result;
        }
    }

    SoundDefinitionImportPlan plan;
    plan.document = destination;
    plan.importedRootDefinitionId = mappedId(
        definitionRemaps, request.rootDefinitionId);
    std::unordered_set<std::string> targetDefinitionIds;
    std::unordered_set<std::string> requiredAssetIds;
    for (const auto* definition : closure)
    {
        auto imported = remappedDefinition(
            *definition, definitionRemaps, assetRemaps);
        if (!targetDefinitionIds.insert(imported.definitionId).second)
        {
            result.diagnostic = "Definition remaps collapse multiple dependencies onto "
                + imported.definitionId;
            return result;
        }
        for (const auto& node : definition->graph.nodes)
        {
            for (const auto& parameter : node.parameters)
            {
                if (parameter.resource.has_value())
                {
                    requiredAssetIds.insert(*parameter.resource);
                    if (requiredAssetIds.size()
                        > maximumSoundDefinitionImportAssets)
                    {
                        result.diagnostic =
                            "Reusable definition import exceeds the bounded asset closure";
                        return result;
                    }
                }
            }
        }
        for (const auto& parameter : definition->exposedParameters)
        {
            if (parameter.defaultValue.resource.has_value())
            {
                requiredAssetIds.insert(*parameter.defaultValue.resource);
                if (requiredAssetIds.size()
                    > maximumSoundDefinitionImportAssets)
                {
                    result.diagnostic =
                        "Reusable definition import exceeds the bounded asset closure";
                    return result;
                }
            }
        }
        const auto* existing = findById(
            plan.document.soundDefinitions,
            imported.definitionId,
            &ProjectSoundDefinition::definitionId);
        if (existing != nullptr)
        {
            if (*existing != imported)
            {
                result.diagnostic = "Definition identity collision requires an explicit non-conflicting remap: "
                    + imported.definitionId;
                return result;
            }
            continue;
        }
        plan.importedDefinitionIds.push_back(imported.definitionId);
        plan.document.soundDefinitions.push_back(std::move(imported));
    }

    for (const auto& remap : request.assetRemaps)
    {
        if (!requiredAssetIds.contains(remap.sourceId))
        {
            result.diagnostic =
                "Asset collision plan references content outside the import closure: "
                + remap.sourceId;
            return result;
        }
    }

    std::vector<std::string> orderedAssetIds(requiredAssetIds.begin(), requiredAssetIds.end());
    std::ranges::sort(orderedAssetIds);
    std::unordered_set<std::string> targetAssetIds;
    for (const auto& sourceAssetId : orderedAssetIds)
    {
        const auto* sourceAsset = findById(
            source.assets, sourceAssetId, &ProjectAsset::id);
        if (sourceAsset == nullptr)
        {
            result.diagnostic = "Reusable definition import is missing asset "
                + sourceAssetId;
            return result;
        }
        auto imported = *sourceAsset;
        imported.id = mappedId(assetRemaps, sourceAssetId);
        if (!targetAssetIds.insert(imported.id).second)
        {
            result.diagnostic =
                "Asset remaps collapse multiple dependencies onto "
                + imported.id;
            return result;
        }
        if (request.sourceProjectFilePath.has_value())
        {
            const auto resolved = resolveProjectAssetPath(
                imported, request.sourceProjectFilePath);
            if (resolved.empty() || resolved.is_relative())
            {
                result.diagnostic =
                    "Reusable definition import cannot resolve asset path "
                    + sourceAssetId;
                return result;
            }
            imported.path = projectAssetStoredPath(
                resolved, request.destinationProjectFilePath);
        }
        const auto* existing = findById(
            plan.document.assets, imported.id, &ProjectAsset::id);
        if (existing != nullptr)
        {
            if (*existing != imported)
            {
                result.diagnostic = "Asset identity collision requires an explicit non-conflicting remap: "
                    + imported.id;
                return result;
            }
            continue;
        }
        plan.importedAssetIds.push_back(imported.id);
        plan.document.assets.push_back(std::move(imported));
    }

    const auto validation = validateProjectDocumentForEditing(
        plan.document, registry);
    if (!validation.ok())
    {
        result.diagnostic = validation.errors.empty()
            ? "Imported reusable definition does not form a valid editable project"
            : validation.errors.front().message;
        return result;
    }
    plan.noOp = plan.importedDefinitionIds.empty()
        && plan.importedAssetIds.empty();
    result.plan = std::move(plan);
    return result;
}

SoundDefinitionImportCollisionPreview suggestSoundDefinitionImportCollisionPlan(
    const ProjectDocument& destination,
    const ProjectDocument& source,
    std::string_view rootDefinitionId,
    const talas::core::NodeRegistry& registry,
    std::optional<std::filesystem::path> sourceProjectFilePath,
    std::optional<std::filesystem::path> destinationProjectFilePath)
{
    SoundDefinitionImportCollisionPreview preview;
    SoundDefinitionImportRequest request;
    request.rootDefinitionId = std::string{rootDefinitionId};
    request.sourceProjectFilePath = std::move(sourceProjectFilePath);
    request.destinationProjectFilePath = std::move(destinationProjectFilePath);

    std::vector<const ProjectSoundDefinition*> closure;
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> requiredAssetIds;
    const auto collect = [&](auto&& self, std::string_view definitionId) -> bool {
        if (visited.contains(std::string{definitionId}))
            return true;
        if (visiting.size() + visited.size()
            >= maximumSoundDefinitionImportClosure)
        {
            preview.diagnostic =
                "Reusable definition import exceeds the bounded dependency closure";
            return false;
        }
        if (!visiting.insert(std::string{definitionId}).second)
        {
            preview.diagnostic =
                "Reusable definition import contains a dependency cycle at "
                + std::string{definitionId};
            return false;
        }
        const auto* definition = findById(
            source.soundDefinitions,
            definitionId,
            &ProjectSoundDefinition::definitionId);
        if (definition == nullptr)
        {
            preview.diagnostic =
                "Reusable definition import is missing dependency "
                + std::string{definitionId};
            return false;
        }
        for (const auto& node : definition->graph.nodes)
        {
            if (node.soundDefinitionId.has_value()
                && !self(self, *node.soundDefinitionId))
                return false;
            for (const auto& parameter : node.parameters)
            {
                if (parameter.resource.has_value())
                {
                    requiredAssetIds.insert(*parameter.resource);
                    if (requiredAssetIds.size()
                        > maximumSoundDefinitionImportAssets)
                    {
                        preview.diagnostic =
                            "Reusable definition import exceeds the bounded asset closure";
                        return false;
                    }
                }
            }
        }
        for (const auto& parameter : definition->exposedParameters)
        {
            if (parameter.defaultValue.resource.has_value())
            {
                requiredAssetIds.insert(*parameter.defaultValue.resource);
                if (requiredAssetIds.size()
                    > maximumSoundDefinitionImportAssets)
                {
                    preview.diagnostic =
                        "Reusable definition import exceeds the bounded asset closure";
                    return false;
                }
            }
        }
        visiting.erase(std::string{definitionId});
        visited.insert(std::string{definitionId});
        closure.push_back(definition);
        return true;
    };
    if (rootDefinitionId.empty() || !collect(collect, rootDefinitionId))
    {
        if (preview.diagnostic.empty())
            preview.diagnostic = "Reusable definition import requires one exact root identity";
        return preview;
    }

    std::unordered_map<std::string, std::string> assetRemaps;
    std::vector<std::string> orderedAssets(
        requiredAssetIds.begin(), requiredAssetIds.end());
    std::ranges::sort(orderedAssets);
    for (const auto& assetId : orderedAssets)
    {
        const auto* sourceAsset = findById(
            source.assets, assetId, &ProjectAsset::id);
        if (sourceAsset == nullptr)
        {
            preview.diagnostic =
                "Reusable definition import is missing asset " + assetId;
            return preview;
        }
        auto importedAsset = *sourceAsset;
        if (request.sourceProjectFilePath.has_value())
        {
            const auto resolved = resolveProjectAssetPath(
                importedAsset, request.sourceProjectFilePath);
            if (resolved.empty() || resolved.is_relative())
            {
                preview.diagnostic =
                    "Reusable definition import cannot resolve asset path "
                    + assetId;
                return preview;
            }
            importedAsset.path = projectAssetStoredPath(
                resolved, request.destinationProjectFilePath);
        }
        const auto* existing = findById(
            destination.assets, assetId, &ProjectAsset::id);
        if (existing == nullptr || *existing == importedAsset)
            continue;
        const auto destinationId = nextImportedIdentity(
            assetId,
            [&](std::string_view candidate) {
                return requiredAssetIds.contains(std::string{candidate})
                    || findById(destination.assets,
                           candidate,
                           &ProjectAsset::id)
                        != nullptr
                    || std::ranges::any_of(
                        request.assetRemaps,
                        [&](const auto& remap) {
                            return remap.destinationId == candidate;
                        });
            });
        request.assetRemaps.push_back({assetId, destinationId});
        assetRemaps.emplace(assetId, destinationId);
    }

    std::unordered_map<std::string, std::string> definitionRemaps;
    for (const auto* definition : closure)
    {
        const auto imported = remappedDefinition(
            *definition, definitionRemaps, assetRemaps);
        const auto* existing = findById(
            destination.soundDefinitions,
            imported.definitionId,
            &ProjectSoundDefinition::definitionId);
        if (existing == nullptr || *existing == imported)
            continue;
        const auto destinationId = nextImportedIdentity(
            definition->definitionId,
            [&](std::string_view candidate) {
                return visited.contains(std::string{candidate})
                    || findById(destination.soundDefinitions,
                           candidate,
                           &ProjectSoundDefinition::definitionId)
                        != nullptr
                    || std::ranges::any_of(
                        request.definitionRemaps,
                        [&](const auto& remap) {
                            return remap.destinationId == candidate;
                        });
            });
        request.definitionRemaps.push_back(
            {definition->definitionId, destinationId});
        definitionRemaps.emplace(definition->definitionId, destinationId);
    }

    const auto planned = planSoundDefinitionImport(
        destination, source, request, registry);
    if (!planned.ok())
    {
        preview.diagnostic = planned.diagnostic;
        return preview;
    }
    preview.request = std::move(request);
    return preview;
}
}
