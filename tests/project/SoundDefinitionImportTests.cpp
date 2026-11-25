#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/SoundDefinitionImport.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectSoundDefinition voice(std::string id, std::string assetId)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(id);
    definition.displayName = "Imported Voice";
    talas::project::ProjectParameterExpression wavetable;
    wavetable.parameterId = "wavetable";
    wavetable.resource = std::move(assetId);
    definition.graph.nodes.push_back(
        {"osc", "talas.oscillator", {std::move(wavetable)}});
    definition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    return definition;
}

talas::project::ProjectSoundDefinition layer()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "layer";
    definition.displayName = "Imported Layer";
    talas::project::ProjectNode instance;
    instance.instanceId = "voiceInstance";
    instance.soundDefinitionId = "voice";
    definition.graph.nodes.push_back(std::move(instance));
    definition.outputs.push_back(
        {"audioOut", talas::core::PortType::Audio,
            "voiceInstance", "audioOut"});
    return definition;
}

talas::project::ProjectDocument sourceDocument()
{
    talas::project::ProjectDocument source;
    source.soundDefinitions = {voice("voice", "Tables/Wave"), layer()};
    source.assets.push_back({
        "Tables/Wave",
        talas::project::ProjectAssetKind::Table,
        "Assets/wave.txt",
    });
    return source;
}

void testTransitiveImportIsOneUndoAndIdenticalImportIsNoOp()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument destination;
    const auto source = sourceDocument();
    const auto planned = talas::project::planSoundDefinitionImport(
        destination, source, {"layer"}, registry);
    require(planned.ok()
            && !planned.plan->noOp
            && planned.plan->importedDefinitionIds
                == std::vector<std::string>({"voice", "layer"})
            && planned.plan->importedAssetIds
                == std::vector<std::string>({"Tables/Wave"}),
        "import should close over definitions and exact resource assets in dependency order");

    talas::project::ProjectEditor editor(destination, registry);
    const auto applied = editor.applyDocumentEdit(planned.plan->document);
    require(applied.ok() && editor.document().soundDefinitions.size() == 2
            && editor.document().assets.size() == 1,
        "one prepared import should commit as one document edit");
    require(editor.undo().ok()
            && editor.document().soundDefinitions.empty()
            && editor.document().assets.empty(),
        "one Undo should remove the complete imported closure");
    require(editor.redo().ok(), "Redo should restore the complete import");

    const auto second = talas::project::planSoundDefinitionImport(
        editor.document(), source, {"layer"}, registry);
    require(second.ok() && second.plan->noOp,
        "byte-identical imported content should be a deterministic no-op");
}

void testConflictsRequireExplicitStableRemaps()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto destination = talas::project::ProjectDocument{};
    destination.soundDefinitions.push_back(voice("voice", "Tables/Other"));
    destination.assets.push_back({
        "Tables/Other",
        talas::project::ProjectAssetKind::Table,
        "Assets/other.txt",
    });
    destination.assets.push_back({
        "Tables/Wave",
        talas::project::ProjectAssetKind::Table,
        "Assets/different.txt",
    });
    const auto source = sourceDocument();
    const auto rejected = talas::project::planSoundDefinitionImport(
        destination, source, {"layer"}, registry);
    require(!rejected.ok()
            && rejected.diagnostic.find("collision") != std::string::npos,
        "conflicting reusable identities should fail before document mutation");

    talas::project::SoundDefinitionImportRequest request;
    request.rootDefinitionId = "layer";
    request.definitionRemaps = {{"voice", "voiceImported"},
        {"layer", "layerImported"}};
    request.assetRemaps = {{"Tables/Wave", "Tables/WaveImported"}};
    const auto remapped = talas::project::planSoundDefinitionImport(
        destination, source, request, registry);
    require(remapped.ok()
            && remapped.plan->importedDefinitionIds
                == std::vector<std::string>({"voiceImported", "layerImported"})
            && remapped.plan->importedAssetIds
                == std::vector<std::string>({"Tables/WaveImported"})
            && remapped.plan->importedRootDefinitionId == "layerImported"
            && remapped.plan->document.soundDefinitions.back()
                    .graph.nodes.front().soundDefinitionId
                == std::optional<std::string>{"voiceImported"},
        "explicit remaps should rewrite the complete dependency and asset closure deterministically: "
            + remapped.diagnostic);

    const auto suggested =
        talas::project::suggestSoundDefinitionImportCollisionPlan(
            destination, source, "layer", registry);
    require(suggested.ok()
            && suggested.request->definitionRemaps
                == std::vector<talas::project::SoundDefinitionImportIdentityRemap>{
                    {"voice", "voiceImported"}}
            && suggested.request->assetRemaps
                == std::vector<talas::project::SoundDefinitionImportIdentityRemap>{
                    {"Tables/Wave", "Tables/WaveImported"}},
        "collision preview should derive the exact stable remap plan without mutating either project");
    const auto suggestedPlan = talas::project::planSoundDefinitionImport(
        destination, source, *suggested.request, registry);
    require(suggestedPlan.ok()
            && suggestedPlan.plan->importedRootDefinitionId == "layer",
        "suggested collision preview should produce a directly executable import plan with exact root identity");
}

void testMissingDependencyAndAssetFailWithoutPartialOutput()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto source = sourceDocument();
    source.soundDefinitions.erase(source.soundDefinitions.begin());
    const auto missingDependency = talas::project::planSoundDefinitionImport(
        {}, source, {"layer"}, registry);
    require(!missingDependency.ok() && !missingDependency.plan.has_value(),
        "missing transitive definition should expose no partial import plan");

    source = sourceDocument();
    source.assets.clear();
    const auto missingAsset = talas::project::planSoundDefinitionImport(
        {}, source, {"layer"}, registry);
    require(!missingAsset.ok() && !missingAsset.plan.has_value(),
        "missing transitive asset should expose no partial import plan");

    source = sourceDocument();
    source.soundDefinitions.front().graph.nodes.push_back({
        "cycle", "", {}, std::optional<std::string>{"layer"}});
    const auto cycle = talas::project::planSoundDefinitionImport(
        {}, source, {"layer"}, registry);
    require(!cycle.ok() && !cycle.plan.has_value()
            && cycle.diagnostic.find("cycle") != std::string::npos,
        "recursive import dependency should fail without partial output");

    source = sourceDocument();
    talas::project::SoundDefinitionImportRequest unused;
    unused.rootDefinitionId = "layer";
    unused.definitionRemaps = {{"unrelated", "unrelatedImported"}};
    const auto unusedDefinition = talas::project::planSoundDefinitionImport(
        {}, source, unused, registry);
    require(!unusedDefinition.ok() && !unusedDefinition.plan.has_value(),
        "collision plan should reject definition remaps outside the exact closure");
    unused.definitionRemaps.clear();
    unused.assetRemaps = {{"Unused/Asset", "Unused/AssetImported"}};
    const auto unusedAsset = talas::project::planSoundDefinitionImport(
        {}, source, unused, registry);
    require(!unusedAsset.ok() && !unusedAsset.plan.has_value(),
        "collision plan should reject asset remaps outside the exact closure");
}

void testAssetClosurePreservesPhysicalPathAcrossProjectContexts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = sourceDocument();
    const auto sourceProject =
        std::filesystem::path{"/catalog/examples/source.talas.json"};
    const auto destinationProject =
        std::filesystem::path{"/sessions/current/project.talas.json"};
    talas::project::SoundDefinitionImportRequest request;
    request.rootDefinitionId = "layer";
    request.sourceProjectFilePath = sourceProject;
    request.destinationProjectFilePath = destinationProject;
    const auto planned = talas::project::planSoundDefinitionImport(
        {}, source, request, registry);
    require(planned.ok() && planned.plan->document.assets.size() == 1,
        "asset-aware import should produce the complete reusable closure");
    require(talas::project::resolveProjectAssetPath(
                planned.plan->document.assets.front(), destinationProject)
            == talas::project::resolveProjectAssetPath(
                source.assets.front(), sourceProject),
        "imported relative assets should retain exact physical identity in the destination context");
}

void testClosureBoundsAndIdentityCollapseReject()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto collapsedSource = sourceDocument();
    talas::project::ProjectParameterExpression secondAsset;
    secondAsset.parameterId = "secondAsset";
    secondAsset.resource = "Tables/WaveImported";
    collapsedSource.soundDefinitions.front().graph.nodes.front()
        .parameters.push_back(std::move(secondAsset));
    collapsedSource.assets.push_back({
        "Tables/WaveImported",
        talas::project::ProjectAssetKind::Table,
        "Assets/other-wave.txt",
    });
    talas::project::SoundDefinitionImportRequest collapsingRequest;
    collapsingRequest.rootDefinitionId = "layer";
    collapsingRequest.assetRemaps = {
        {"Tables/Wave", "Tables/WaveImported"}};
    const auto collapsed = talas::project::planSoundDefinitionImport(
        {}, collapsedSource, collapsingRequest, registry);
    require(!collapsed.ok()
            && collapsed.diagnostic.find("collapse") != std::string::npos,
        "asset remaps must not collapse two distinct closure identities");

    talas::project::ProjectDocument oversized;
    oversized.soundDefinitions.reserve(
        talas::project::maximumSoundDefinitionImportClosure + 1);
    for (std::size_t index = 0;
         index <= talas::project::maximumSoundDefinitionImportClosure;
         ++index)
    {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = "d" + std::to_string(index);
        if (index < talas::project::maximumSoundDefinitionImportClosure)
        {
            talas::project::ProjectNode nested;
            nested.instanceId = "nested";
            nested.soundDefinitionId = "d" + std::to_string(index + 1);
            definition.graph.nodes.push_back(std::move(nested));
        }
        oversized.soundDefinitions.push_back(std::move(definition));
    }
    const auto bounded = talas::project::planSoundDefinitionImport(
        {}, oversized, {"d0"}, registry);
    require(!bounded.ok()
            && bounded.diagnostic.find("bounded dependency closure")
                != std::string::npos,
        "dependency planning must reject before traversing an oversized closure");
}
}

int main()
{
    testTransitiveImportIsOneUndoAndIdenticalImportIsNoOp();
    testConflictsRequireExplicitStableRemaps();
    testMissingDependencyAndAssetFailWithoutPartialOutput();
    testAssetClosurePreservesPhysicalPathAcrossProjectContexts();
    testClosureBoundsAndIdentityCollapseReject();
    return 0;
}
