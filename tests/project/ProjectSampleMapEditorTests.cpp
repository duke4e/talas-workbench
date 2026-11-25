#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

talas::project::ProjectEditor makeEditor()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 'a'));
    document.assets.push_back(audioAsset("bassSample", 'b'));
    return {std::move(document), builtinRegistry()};
}

talas::project::ProjectSampleMap map(
    std::string id,
    std::string name)
{
    talas::project::ProjectSampleMap result;
    result.mapId = std::move(id);
    result.displayName = std::move(name);
    return result;
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

talas::project::ProjectEditor makePopulatedEditor()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 'a'));
    document.assets.push_back(audioAsset("bassSample", 'b'));
    auto mainMap = map("mainMap", "Main");
    mainMap.regions.push_back(
        region("leadRegion", "leadSample", 60, 48, 72));
    mainMap.regions.push_back(
        region("bassRegion", "bassSample", 36, 24, 47));
    document.sampleMapLibrary.maps.push_back(std::move(mainMap));
    return {std::move(document), builtinRegistry()};
}

void testMapCrudAndOrderAreFailureAtomicAndUndoable()
{
    auto editor = makeEditor();
    requireOk(
        editor.addSampleMap(map("leadMap", "Lead")),
        "adding the first sample map");
    requireOk(
        editor.addSampleMap(map("bassMap", "Bass")),
        "adding the second sample map");
    require(
        editor.document().sampleMapLibrary.maps.size() == 2,
        "both sample maps should commit");

    const auto beforeDuplicate = editor.document().sampleMapLibrary;
    requireError(
        editor.addSampleMap(map("leadMap", "Duplicate")),
        talas::project::EditError::DuplicateSampleMap,
        "adding a duplicate sample-map identity");
    require(
        editor.document().sampleMapLibrary == beforeDuplicate,
        "a duplicate map should publish no partial edit");

    requireOk(
        editor.renameSampleMap("leadMap", "Lead Renamed"),
        "renaming a sample map");
    require(
        editor.document().sampleMapLibrary.maps.front().displayName
            == "Lead Renamed",
        "map rename should preserve order and identity");
    requireOk(editor.undo(), "one undo should revert map rename");
    require(
        editor.document().sampleMapLibrary.maps.front().displayName == "Lead",
        "map rename undo should restore the complete prior map");
    requireOk(editor.redo(), "one redo should restore map rename");

    requireOk(
        editor.moveSampleMap("leadMap", 1),
        "moving a sample map to an exact destination index");
    require(
        editor.document().sampleMapLibrary.maps.front().mapId == "bassMap"
            && editor.document().sampleMapLibrary.maps.back().mapId
                == "leadMap",
        "map reorder should preserve stable identities");
    requireOk(editor.undo(), "one undo should revert map reorder");
    require(
        editor.document().sampleMapLibrary.maps.front().mapId == "leadMap",
        "map reorder undo should restore the previous order");
    requireOk(editor.redo(), "one redo should restore map reorder");

    const auto beforeBadMove = editor.document().sampleMapLibrary;
    requireError(
        editor.moveSampleMap("leadMap", 2),
        talas::project::EditError::InvalidSampleMap,
        "moving a sample map beyond the collection");
    require(
        editor.document().sampleMapLibrary == beforeBadMove,
        "an invalid map move should be failure-atomic");

    requireOk(
        editor.removeSampleMap("leadMap"),
        "removing a known sample map");
    require(
        editor.document().sampleMapLibrary.maps.size() == 1
            && editor.document().assets.size() == 2,
        "map removal should never remove manifest assets");
    requireOk(editor.undo(), "one undo should restore removed map");
    require(
        editor.document().sampleMapLibrary.maps.size() == 2
            && editor.document().assets.size() == 2,
        "map removal undo should restore the map without changing assets");

    const auto beforeUnknown = editor.document().sampleMapLibrary;
    requireError(
        editor.removeSampleMap("missingMap"),
        talas::project::EditError::UnknownSampleMap,
        "removing an unknown sample map");
    require(
        editor.document().sampleMapLibrary == beforeUnknown,
        "unknown map removal should publish nothing");
}

void testRegionCrudAndOrderAreFailureAtomicAndUndoable()
{
    auto editor = makeEditor();
    requireOk(
        editor.addSampleMap(map("mainMap", "Main")),
        "region fixture should add its map");
    requireOk(
        editor.addSampleRegion(
            "mainMap",
            region("leadRegion", "leadSample", 60, 48, 72)),
        "adding the first sample region");
    requireOk(
        editor.addSampleRegion(
            "mainMap",
            region("bassRegion", "bassSample", 36, 24, 47)),
        "adding the second sample region");

    const auto beforeDuplicate = editor.document().sampleMapLibrary;
    requireError(
        editor.addSampleRegion(
            "mainMap",
            region("leadRegion", "bassSample", 60, 48, 72)),
        talas::project::EditError::DuplicateSampleRegion,
        "adding a duplicate region identity");
    require(
        editor.document().sampleMapLibrary == beforeDuplicate,
        "duplicate region add should publish nothing");

    auto updated = editor.document().sampleMapLibrary.maps.front().regions.front();
    updated.reverse = true;
    updated.lowVelocity = 17;
    updated.highVelocity = 112;
    updated.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        100,
        400,
    };
    requireOk(
        editor.updateSampleRegion("mainMap", "leadRegion", updated),
        "updating exact sample-region fields");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.front()
            == updated,
        "region update should commit every portable field");
    requireOk(editor.undo(), "one undo should revert region update");
    require(
        !editor.document().sampleMapLibrary.maps.front().regions.front().reverse,
        "region update undo should restore the prior region");
    requireOk(editor.redo(), "one redo should restore region update");

    requireOk(
        editor.moveSampleRegion("mainMap", "leadRegion", 1),
        "moving a sample region to an exact layer index");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.front().regionId
            == "bassRegion",
        "region reorder should define deterministic layer order");
    requireOk(editor.undo(), "one undo should revert region reorder");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.front().regionId
            == "leadRegion",
        "region reorder undo should restore layer order");
    requireOk(editor.redo(), "one redo should restore region reorder");

    requireOk(
        editor.removeSampleRegion("mainMap", "leadRegion"),
        "removing one region");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.size() == 1
            && editor.document().assets.size() == 2,
        "region removal should leave every manifest asset intact");
    requireOk(editor.undo(), "one undo should restore removed region");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.size() == 2
            && editor.document().assets.size() == 2,
        "region removal undo should restore only project map state");

    const auto beforeUnknown = editor.document().sampleMapLibrary;
    requireError(
        editor.updateSampleRegion(
            "mainMap",
            "missingRegion",
            region("missingRegion", "leadSample", 60, 48, 72)),
        talas::project::EditError::UnknownSampleRegion,
        "updating an unknown sample region");
    require(
        editor.document().sampleMapLibrary == beforeUnknown,
        "unknown region update should publish nothing");
}

void testMultiRegionUpdatesAndRemovalAreSingleUndoEdits()
{
    auto editor = makeEditor();
    requireOk(
        editor.addSampleMap(map("mainMap", "Main")),
        "batch fixture should add its map");
    requireOk(
        editor.addSampleRegion(
            "mainMap",
            region("leadRegion", "leadSample", 60, 48, 72)),
        "batch fixture should add lead region");
    requireOk(
        editor.addSampleRegion(
            "mainMap",
            region("bassRegion", "bassSample", 36, 24, 47)),
        "batch fixture should add bass region");

    const auto beforeBatch = editor.document().sampleMapLibrary;
    auto lead = beforeBatch.maps.front().regions[0];
    auto bass = beforeBatch.maps.front().regions[1];
    lead.lowNote = 50;
    lead.highNote = 74;
    bass.lowNote = 20;
    bass.highNote = 43;
    requireOk(
        editor.updateSampleRegions("mainMap", {lead, bass}),
        "updating a multi-selection as one batch");
    require(
        editor.document().sampleMapLibrary.maps.front().regions[0] == lead
            && editor.document().sampleMapLibrary.maps.front().regions[1]
                == bass,
        "batch update should publish every selected region together");
    requireOk(editor.undo(), "one undo should revert the full batch update");
    require(
        editor.document().sampleMapLibrary == beforeBatch,
        "one undo should restore every region changed by the batch");
    requireOk(editor.redo(), "one redo should restore the full batch update");

    const auto beforeInvalidBatch = editor.document().sampleMapLibrary;
    auto missing = lead;
    missing.regionId = "missingRegion";
    requireError(
        editor.updateSampleRegions("mainMap", {lead, missing}),
        talas::project::EditError::UnknownSampleRegion,
        "batch update containing an unknown region");
    require(
        editor.document().sampleMapLibrary == beforeInvalidBatch,
        "one unknown batch member should roll back the complete batch");

    auto invalid = lead;
    invalid.lowNote = 90;
    requireError(
        editor.updateSampleRegions("mainMap", {invalid, bass}),
        talas::project::EditError::InvalidSampleMap,
        "batch update containing invalid region bounds");
    require(
        editor.document().sampleMapLibrary == beforeInvalidBatch,
        "one invalid batch member should roll back the complete batch");

    requireOk(
        editor.removeSampleRegions(
            "mainMap",
            {"leadRegion", "bassRegion"}),
        "removing a multi-selection as one batch");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.empty()
            && editor.document().assets.size() == 2,
        "batch removal should retain manifest assets");
    requireOk(editor.undo(), "one undo should restore batch-removed regions");
    require(
        editor.document().sampleMapLibrary == beforeInvalidBatch,
        "one undo should restore every batch-removed region in original order");
}

void testBatchCommandsHaveExactlyOneHistoryEntry()
{
    {
        auto editor = makePopulatedEditor();
        const auto before = editor.document().sampleMapLibrary;
        auto lead = before.maps.front().regions[0];
        auto bass = before.maps.front().regions[1];
        lead.lowNote = 50;
        lead.highNote = 74;
        bass.lowNote = 20;
        bass.highNote = 43;

        requireOk(
            editor.updateSampleRegions("mainMap", {lead, bass}),
            "clean-history batch update");
        const auto after = editor.document().sampleMapLibrary;
        requireOk(editor.undo(), "batch update should expose one undo");
        require(
            editor.document().sampleMapLibrary == before,
            "batch update undo should restore the exact initial state");
        requireError(
            editor.undo(),
            talas::project::EditError::NoUndoAvailable,
            "batch update should not expose a second undo");
        requireOk(editor.redo(), "batch update should expose one redo");
        require(
            editor.document().sampleMapLibrary == after,
            "batch update redo should restore the exact edited state");
        requireError(
            editor.redo(),
            talas::project::EditError::NoRedoAvailable,
            "batch update should not expose a second redo");
    }

    {
        auto editor = makePopulatedEditor();
        const auto before = editor.document().sampleMapLibrary;
        requireOk(
            editor.removeSampleRegions(
                "mainMap",
                {"leadRegion", "bassRegion"}),
            "clean-history batch removal");
        requireOk(editor.undo(), "batch removal should expose one undo");
        require(
            editor.document().sampleMapLibrary == before,
            "batch removal undo should restore exact region order");
        requireError(
            editor.undo(),
            talas::project::EditError::NoUndoAvailable,
            "batch removal should not expose a second undo");
    }
}

void testNoOpCommandsDoNotCreateHistory()
{
    auto editor = makePopulatedEditor();
    const auto lead =
        editor.document().sampleMapLibrary.maps.front().regions.front();
    requireOk(
        editor.renameSampleMap("mainMap", "Main"),
        "identical map rename should succeed as a no-op");
    requireOk(
        editor.moveSampleMap("mainMap", 0),
        "identical map order should succeed as a no-op");
    requireOk(
        editor.updateSampleRegion("mainMap", "leadRegion", lead),
        "identical region update should succeed as a no-op");
    requireOk(
        editor.updateSampleRegions("mainMap", {lead}),
        "identical region batch should succeed as a no-op");
    requireOk(
        editor.moveSampleRegion("mainMap", "leadRegion", 0),
        "identical region order should succeed as a no-op");
    requireOk(
        editor.removeSampleRegions("mainMap", {}),
        "empty region removal should succeed as a no-op");
    require(
        !editor.canUndo() && !editor.canRedo(),
        "no-op sample-map commands should not create history");
}
}

int main()
{
    testMapCrudAndOrderAreFailureAtomicAndUndoable();
    testRegionCrudAndOrderAreFailureAtomicAndUndoable();
    testMultiRegionUpdatesAndRemovalAreSingleUndoEdits();
    testBatchCommandsHaveExactlyOneHistoryEntry();
    testNoOpCommandsDoNotCreateHistory();
    return 0;
}
