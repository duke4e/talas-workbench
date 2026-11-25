#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectSampleMaps.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireOk(
    const talas::project::EditResult& result,
    const std::string& message)
{
    require(result.ok(), message + ": " + result.message);
}

void requireError(
    const talas::project::EditResult& result,
    talas::project::EditError expected,
    const std::string& message)
{
    require(!result.ok(), message + " should fail");
    require(result.error.has_value(), message + " should report an error");
    require(
        *result.error == expected,
        message + " should report the expected error");
}

talas::core::NodeRegistry& builtinRegistry()
{
    static talas::core::NodeRegistry registry = [] {
        talas::core::NodeRegistry builtins;
        talas::core::registerBuiltinNodeTypes(builtins);
        return builtins;
    }();
    return registry;
}

talas::project::ProjectAsset audioAsset(
    std::string id,
    char hashCharacter,
    std::uint64_t frameCount = 1000)
{
    talas::project::ProjectAsset asset;
    asset.id = std::move(id);
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "samples/" + asset.id + ".wav";
    asset.contentHash = std::string(64, hashCharacter);
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = frameCount;
    asset.encoding = "pcm16-wav";
    return asset;
}

talas::project::ProjectSampleRegion region(
    std::string id,
    std::string assetId,
    std::uint8_t rootNote,
    std::uint8_t lowNote,
    std::uint8_t highNote)
{
    talas::project::ProjectSampleRegion result;
    result.regionId = std::move(id);
    result.assetId = std::move(assetId);
    result.rootNote = rootNote;
    result.lowNote = lowNote;
    result.highNote = highNote;
    return result;
}

talas::project::ProjectDocument baseDocument()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 'a'));
    document.assets.push_back(audioAsset("bassSample", 'b'));

    talas::project::ProjectSampleMap map;
    map.mapId = "mainMap";
    map.displayName = "Main";
    auto existing = region(
        "existingRegion",
        "leadSample",
        60,
        48,
        72);
    existing.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        100,
        800,
    };
    map.regions.push_back(std::move(existing));
    document.sampleMapLibrary.maps.push_back(std::move(map));
    return document;
}

talas::project::ProjectEditor makeEditor(
    talas::project::ProjectDocument document = baseDocument())
{
    return {std::move(document), builtinRegistry()};
}

talas::project::ProjectSampleMapAutoMapRequest autoMapRequest()
{
    talas::project::ProjectSampleMapAutoMapRequest request;
    request.lowNote = 36;
    request.highNote = 84;
    request.anchorRootNote = 60;
    request.candidates = {
        {"snareRegion", "snareSample", "Snare.wav", "/samples/Z/Snare.wav"},
        {"kickBRegion", "kickBSample", "KICK.wav", "/samples/B/KICK.wav"},
        {"kickARegion", "kickASample", "kick.wav", "/samples/A/kick.wav"},
    };
    return request;
}

void testAutoMapIsDeterministicAndOneUndo()
{
    const auto mapped = talas::project::makeAutoMappedSampleRegions(
        autoMapRequest());
    require(mapped.ok(), "valid auto-map request should produce a preview");
    require(
        mapped.regions.size() == 3
            && mapped.regions[0].regionId == "kickARegion"
            && mapped.regions[1].regionId == "kickBRegion"
            && mapped.regions[2].regionId == "snareRegion",
        "auto-map should sort by case-folded basename then normalized path");
    require(
        mapped.regions[0].rootNote == 60
            && mapped.regions[1].rootNote == 61
            && mapped.regions[2].rootNote == 62,
        "auto-map should assign ascending roots from the visible anchor");
    require(
        mapped.regions[0].lowNote == 36
            && mapped.regions[0].highNote == 60
            && mapped.regions[1].lowNote == 61
            && mapped.regions[1].highNote == 61
            && mapped.regions[2].lowNote == 62
            && mapped.regions[2].highNote == 84,
        "auto-map should partition the chosen note range at stable midpoints");
    require(
        mapped.regions[0].lowVelocity == 1
            && mapped.regions[0].highVelocity == 127,
        "auto-map should assign the full playable velocity range");

    talas::project::ProjectDocument document;
    document.assets = {
        audioAsset("snareSample", 'c'),
        audioAsset("kickBSample", 'd'),
        audioAsset("kickASample", 'e'),
    };
    talas::project::ProjectSampleMap map;
    map.mapId = "mainMap";
    map.displayName = "Main";
    document.sampleMapLibrary.maps.push_back(std::move(map));
    auto editor = makeEditor(std::move(document));
    const auto before = editor.document().sampleMapLibrary;

    requireOk(
        editor.autoMapSampleRegions("mainMap", autoMapRequest()),
        "applying an auto-map preview");
    require(
        editor.document().sampleMapLibrary.maps.front().regions
            == mapped.regions,
        "editor should publish the exact deterministic preview");
    requireOk(editor.undo(), "auto-map should expose one undo");
    require(
        editor.document().sampleMapLibrary == before,
        "auto-map undo should restore the complete empty map");
    requireError(
        editor.undo(),
        talas::project::EditError::NoUndoAvailable,
        "auto-map should not expose a second undo");

    auto tooMany = autoMapRequest();
    tooMany.lowNote = 60;
    tooMany.highNote = 60;
    tooMany.anchorRootNote = 60;
    const auto rejected =
        talas::project::makeAutoMappedSampleRegions(tooMany);
    require(
        !rejected.ok(),
        "auto-map should reject candidates that do not fit the chosen range");
}

void testMergeIsNondestructiveAndReusesExactBytes()
{
    auto editor = makeEditor();
    const auto before = editor.document();

    auto exactBytesAlias = audioAsset("leadAlias", 'a');
    exactBytesAlias.path = "imports/another-name.wav";
    auto fresh = audioAsset("newSample", 'c');
    talas::project::ProjectSampleMapImportBatch batch;
    batch.assets = {fresh, exactBytesAlias};
    batch.regions = {
        region("newRegion", "newSample", 78, 73, 84),
        region("reuseRegion", "leadAlias", 36, 24, 47),
    };

    requireOk(
        editor.mergeSampleMapImport("mainMap", std::move(batch)),
        "merging an inspected sample-map candidate");
    const auto& mergedMap =
        editor.document().sampleMapLibrary.maps.front();
    require(
        mergedMap.regions.size() == 3
            && mergedMap.regions.front()
                == before.sampleMapLibrary.maps.front().regions.front(),
        "merge should preserve every existing region byte-for-byte");
    require(
        mergedMap.regions[1].regionId == "newRegion"
            && mergedMap.regions[1].assetId == "newSample"
            && mergedMap.regions[2].regionId == "reuseRegion"
            && mergedMap.regions[2].assetId == "leadSample",
        "merge should append stable candidate order and reuse exact-hash identity");
    require(
        editor.document().assets.size() == 3,
        "exact bytes should not create a duplicate manifest asset");

    requireOk(editor.undo(), "merge should expose one undo");
    require(
        editor.document().assets == before.assets
            && editor.document().sampleMapLibrary
                == before.sampleMapLibrary,
        "merge undo should restore map and manifest together");
    requireError(
        editor.undo(),
        talas::project::EditError::NoUndoAvailable,
        "merge should not expose a second undo");

    auto duplicateEditor = makeEditor();
    const auto duplicateBefore = duplicateEditor.document();
    talas::project::ProjectSampleMapImportBatch duplicate;
    duplicate.regions.push_back(region(
        "existingRegion",
        "bassSample",
        36,
        24,
        47));
    requireError(
        duplicateEditor.mergeSampleMapImport(
            "mainMap",
            std::move(duplicate)),
        talas::project::EditError::DuplicateSampleRegion,
        "merge containing an existing region identity");
    require(
        duplicateEditor.document().sampleMapLibrary
                == duplicateBefore.sampleMapLibrary
            && duplicateEditor.document().assets
                == duplicateBefore.assets,
        "failed merge should publish no map or manifest mutation");

    auto invalidEditor = makeEditor();
    const auto invalidBefore = invalidEditor.document();
    talas::project::ProjectSampleMapImportBatch invalid;
    invalid.assets.push_back(audioAsset("invalidSample", 'f'));
    invalid.regions.push_back(region(
        "invalidRegion",
        "invalidSample",
        80,
        20,
        40));
    requireError(
        invalidEditor.mergeSampleMapImport(
            "mainMap",
            std::move(invalid)),
        talas::project::EditError::InvalidSampleMap,
        "merge containing an invalid candidate region");
    require(
        invalidEditor.document().sampleMapLibrary
                == invalidBefore.sampleMapLibrary
            && invalidEditor.document().assets == invalidBefore.assets,
        "post-normalization validation failure should roll back map and manifest together");
}

void testConfirmedReplaceIsRevisionBoundAndFullyUndoable()
{
    auto editor = makeEditor();
    const auto before = editor.document();
    const auto confirmation =
        editor.sampleMapReplaceConfirmation("mainMap");
    require(
        confirmation.has_value()
            && confirmation->mapId() == "mainMap"
            && confirmation->regionCount() == 1,
        "replace confirmation should identify the exact target and region count");

    talas::project::ProjectSampleMapImportBatch replacement;
    replacement.assets.push_back(audioAsset("replacementSample", 'c'));
    replacement.regions.push_back(region(
        "replacementRegion",
        "replacementSample",
        67,
        0,
        127));
    requireOk(
        editor.replaceSampleMapImport(
            "mainMap",
            std::move(replacement),
            *confirmation),
        "replacing a sample map with current typed confirmation");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.size() == 1
            && editor.document().sampleMapLibrary.maps.front().regions.front()
                    .regionId
                == "replacementRegion"
            && editor.document().assets.size() == 3,
        "replace should replace only target regions and retain prior assets");
    requireOk(editor.undo(), "replace should expose one undo");
    require(
        editor.document().sampleMapLibrary == before.sampleMapLibrary
            && editor.document().assets == before.assets,
        "replace undo should restore every prior region and manifest edit");
    requireError(
        editor.undo(),
        talas::project::EditError::NoUndoAvailable,
        "replace should not expose a second undo");

    auto staleEditor = makeEditor();
    const auto stale =
        staleEditor.sampleMapReplaceConfirmation("mainMap");
    require(stale.has_value(), "stale fixture should issue confirmation");
    requireOk(
        staleEditor.renameSampleMap("mainMap", "Changed after confirmation"),
        "stale fixture should advance the target map revision");
    const auto afterRename = staleEditor.document();
    talas::project::ProjectSampleMapImportBatch staleReplacement;
    staleReplacement.regions.push_back(region(
        "newRegion",
        "bassSample",
        36,
        0,
        127));
    requireError(
        staleEditor.replaceSampleMapImport(
            "mainMap",
            std::move(staleReplacement),
            *stale),
        talas::project::EditError::StaleSampleMapConfirmation,
        "replace with stale typed confirmation");
    require(
        staleEditor.document().sampleMapLibrary
                == afterRename.sampleMapLibrary
            && staleEditor.document().assets == afterRename.assets,
        "stale replace should publish nothing");
    require(
        !staleEditor.sampleMapReplaceConfirmation("missingMap").has_value(),
        "unknown maps should not issue replace confirmation");
}

void testMergePublishesReusedAssetDispositionAsOneUndo()
{
    auto editor = makeEditor();
    const auto before = editor.document();
    auto embedded = before.assets.front();
    embedded.path = "Assets/leadSample.wav";
    embedded.disposition =
        talas::project::ProjectAssetDisposition::Embedded;

    talas::project::ProjectSampleMapImportBatch batch;
    batch.assets.push_back(embedded);
    batch.expectedAssets.push_back(before.assets.front());
    batch.regions.push_back(region(
        "embeddedReuseRegion",
        embedded.id,
        84,
        73,
        96));
    requireOk(
        editor.mergeSampleMapImport("mainMap", std::move(batch)),
        "publishing reused asset disposition with imported region");
    require(
        editor.document().assets.front() == embedded
            && editor.document().sampleMapLibrary.maps.front().regions.size()
                == 2,
        "merge should publish the managed path and region atomically");
    requireOk(
        editor.undo(),
        "reused disposition import should expose one undo");
    require(
        editor.document().assets == before.assets
            && editor.document().sampleMapLibrary
                == before.sampleMapLibrary,
        "one undo should restore referenced disposition and prior regions");
    requireError(
        editor.undo(),
        talas::project::EditError::NoUndoAvailable,
        "reused disposition import should not expose a second undo");
}

void testShorterAssetReplacementPreservesLoopAuthority()
{
    auto editor = makeEditor();
    const auto before = editor.document();
    auto shorter = before.assets.front();
    shorter.contentHash = std::string(64, 'd');
    shorter.frameCount = 500;

    requireError(
        editor.acceptProjectAssetReplacement(std::move(shorter)),
        talas::project::EditError::InvalidSampleMap,
        "replacement shorter than a retained sample-map loop");
    require(
        editor.document().assets == before.assets
            && editor.document().sampleMapLibrary
                == before.sampleMapLibrary,
        "shorter replacement should preserve the accepted generation and loops");
}
}

int main()
{
    testAutoMapIsDeterministicAndOneUndo();
    testMergeIsNondestructiveAndReusesExactBytes();
    testConfirmedReplaceIsRevisionBoundAndFullyUndoable();
    testMergePublishesReusedAssetDispositionAsOneUndo();
    testShorterAssetReplacementPreservesLoopAuthority();
    return 0;
}
