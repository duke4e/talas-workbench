#include "talas/project/ProjectPartialTrackEdits.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"

#include <cstdlib>
#include <iostream>
#include <ranges>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

talas::project::PartialTrackAsset sourceFixture()
{
    return {
        48000.0,
        4096,
        {
            {1, 0, {{1, 0.5, 440.0, 10.0},
                    {2, 0.25, 880.0, 20.0}}},
            {1, 1024, {{1, 0.4, 445.0, 11.0},
                       {2, 0.2, 875.0, 19.0}}},
            {2, 2048, {}},
        },
    };
}

talas::project::ProjectPartialTrackEditSet editFixture()
{
    talas::project::ProjectPartialTrackEditSet edits;
    edits.editSetId = "voice_edits";
    edits.assetId = "Analysis/Voice";
    edits.assetGeneration = 3;
    edits.sourceContentHash.assign(64, 'a');
    edits.pointOverrides = {
        {{1, 0, 1},
         256,
         {1, 0.75, 450.0, 12.0},
         talas::project::PartialTrackPointInterpolation::Step,
         false},
        {{1, 1024, 2},
         1024,
         {},
         talas::project::PartialTrackPointInterpolation::Linear,
         true},
    };
    edits.insertedPoints = {
        {"insert_a",
         1,
         1536,
         {3, 0.1, 660.0, 8.0},
         talas::project::PartialTrackPointInterpolation::Linear},
    };
    edits.trackStates = {{2, true, false}, {3, false, true}};
    return edits;
}
}

int main()
{
    const auto source = sourceFixture();
    const auto edits = editFixture();
    require(!talas::project::validateProjectPartialTrackEditSet(edits)
                 .has_value(),
        "valid nondestructive partial edit recipe should validate");
    const auto materialized =
        talas::project::materializeProjectPartialTrackEdits(source, edits);
    require(materialized.ok(),
            materialized.error.has_value()
                ? materialized.error->message
                : "partial edits should materialize");
    require(source == sourceFixture(),
            "materialization must not mutate immutable source bytes/model");
    require(materialized.materialized.asset.frames.size() == 5
            && materialized.materialized.asset.frames[1].sourceFrame == 256
            && materialized.materialized.asset.frames[1].points.front()
                    .amplitude == 0.75
            && materialized.materialized.asset.frames[3].sourceFrame == 1536
            && materialized.materialized.asset.frames[3].points.front()
                    .trackId == 3,
        "materialization should preserve exact source timing while applying move/insert edits");
    require(materialized.materialized.trackStates == edits.trackStates,
            "materialization should preserve exact mute/solo authority");
    const auto movedIdentity = std::ranges::find_if(
        materialized.materialized.pointIdentities,
        [&](const auto& identity) {
            return identity.origin
                    == talas::project::MaterializedPartialTrackPointOrigin::Source
                && identity.source == edits.pointOverrides[0].source;
        });
    const auto insertedIdentity = std::ranges::find_if(
        materialized.materialized.pointIdentities,
        [](const auto& identity) {
            return identity.origin
                    == talas::project::MaterializedPartialTrackPointOrigin::Inserted
                && identity.insertedEditId == "insert_a";
        });
    require(materialized.materialized.pointIdentities.size() == 4
                && movedIdentity
                    != materialized.materialized.pointIdentities.end()
                && insertedIdentity
                    != materialized.materialized.pointIdentities.end(),
            "materialization should retain exact source/inserted identities parallel to output points");

    talas::project::ProjectAsset manifest;
    manifest.id = edits.assetId;
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "voice.talaspartials";
    manifest.contentHash = edits.sourceContentHash;
    manifest.generation = edits.assetGeneration;
    manifest.sampleRate = source.sampleRate;
    manifest.frameCount = source.sourceFrameCount;
    manifest.encoding = std::string{talas::project::partialTrackAssetEncoding};
    require(talas::project::partialTrackEditSetMatchesAsset(edits, manifest),
            "edit set should bind exact asset kind/id/generation/hash/shape");
    ++manifest.generation;
    require(!talas::project::partialTrackEditSetMatchesAsset(edits, manifest),
            "asset generation replacement should invalidate old edits");
    --manifest.generation;

    talas::project::ProjectDocument document;
    document.assets.push_back(manifest);
    document.partialTrackEdits.push_back(edits);
    const auto json = talas::project::serializeProjectDocument(document);
    const auto parsed = talas::project::parseProjectDocument(json);
    require(parsed.ok(),
            parsed.errors.empty()
                ? "format-22 partial edits should reopen"
                : parsed.errors.front().path + ": "
                    + parsed.errors.front().message);
    require(parsed.document.partialTrackEdits == document.partialTrackEdits,
            "format-22 partial edits should round-trip exactly");
    auto fractional = json;
    const auto frameToken = fractional.find("\"outputFrame\": 256");
    require(frameToken != std::string::npos,
            "rounding-trap fixture should find output frame");
    fractional.replace(
        frameToken,
        std::string{"\"outputFrame\": 256"}.size(),
        "\"outputFrame\": 256.00000000000000001");
    require(!talas::project::parseProjectDocument(fractional).ok(),
            "partial edit uint64 authority should reject fractional JSON tokens");

    talas::core::NodeRegistry registry;
    talas::project::ProjectDocument editorDocument;
    editorDocument.assets.push_back(manifest);
    talas::project::ProjectEditor editor{editorDocument, registry};
    require(editor.upsertPartialTrackEditSet(edits).ok(),
            "editor should add an exact partial-track edit set");
    require(editor.document().partialTrackEdits.size() == 1
                && editor.document().assets.front() == manifest,
            "editor edit must preserve the immutable source asset manifest");
    auto changed = edits;
    changed.trackStates = {{1, true, false}};
    require(editor.upsertPartialTrackEditSet(changed).ok(),
            "editor should replace the same exact edit-set identity");
    require(editor.undo().ok()
                && editor.document().partialTrackEdits.front() == edits,
            "partial-track edit replacement should be one undo step");
    auto stale = edits;
    ++stale.assetGeneration;
    require(!editor.upsertPartialTrackEditSet(stale).ok(),
            "editor should reject edits bound to stale asset authority");
    require(editor.removePartialTrackEditSet(edits.editSetId).ok()
                && editor.document().partialTrackEdits.empty(),
            "editor should remove an exact partial-track edit set");

    talas::project::ProjectPartialTrackEditBatch batch;
    batch.editSetId = edits.editSetId;
    batch.assetId = edits.assetId;
    batch.assetGeneration = edits.assetGeneration;
    batch.sourceContentHash = edits.sourceContentHash;
    batch.pointMutations = {
        {talas::project::ProjectPartialTrackPointMutationKind::Update,
         {talas::project::MaterializedPartialTrackPointOrigin::Source,
          {1, 0, 1},
          {}},
         1,
         512,
         {4, 0.8, 460.0, 9.0},
         talas::project::PartialTrackPointInterpolation::Step},
        {talas::project::ProjectPartialTrackPointMutationKind::Update,
         {talas::project::MaterializedPartialTrackPointOrigin::Source,
          {1, 1024, 1},
          {}},
         1,
         1280,
         {4, 0.6, 470.0, 8.0},
         talas::project::PartialTrackPointInterpolation::Linear},
        {talas::project::ProjectPartialTrackPointMutationKind::Delete,
         {talas::project::MaterializedPartialTrackPointOrigin::Source,
          {1, 0, 2},
          {}}},
        {talas::project::ProjectPartialTrackPointMutationKind::Insert,
         {talas::project::MaterializedPartialTrackPointOrigin::Inserted,
          {},
          "insert_b"},
         1,
         1792,
         {4, 0.4, 480.0, 7.0},
         talas::project::PartialTrackPointInterpolation::Linear},
    };
    batch.trackStateMutations = {{4, false, true}};
    const auto gesture = editor.beginOwnedHistoryTransaction();
    require(editor.applyPartialTrackEditBatch(source, batch, gesture).ok(),
            "shared partial-track batch command should atomically update, delete, insert, retrack, and solo");
    editor.endOwnedHistoryTransaction(gesture);
    require(source == sourceFixture(),
            "partial-track batch command must not change immutable source bytes");
    const auto editedBatch = talas::project::materializeProjectPartialTrackEdits(
        source, editor.document().partialTrackEdits.front());
    const auto editedJson = talas::project::serializeProjectDocument(
        editor.document());
    const auto reopenedBatch = talas::project::parseProjectDocument(editedJson);
    const auto reopenedMaterialized = reopenedBatch.ok()
        ? talas::project::materializeProjectPartialTrackEdits(
            source, reopenedBatch.document.partialTrackEdits.front())
        : talas::project::MaterializedPartialTrackEditsResult{};
    require(editedBatch.ok()
                && reopenedBatch.ok()
                && reopenedMaterialized.ok()
                && reopenedMaterialized.materialized
                    == editedBatch.materialized
                && editedBatch.materialized.asset.frames.size() == 6
                && editedBatch.materialized.trackStates
                    == std::vector<talas::project::ProjectPartialTrackState>{
                        {4, false, true}},
            "shared batch command should round-trip and materialize one canonical nondestructive recipe");
    require(editor.undo().ok()
                && editor.document().partialTrackEdits.empty(),
            "owned partial-track gesture should collapse into one undo step");
    const auto staleGesture = editor.beginOwnedHistoryTransaction();
    editor.endOwnedHistoryTransaction(staleGesture);
    require(!editor.applyPartialTrackEditBatch(
                 source, batch, staleGesture).ok(),
            "stale partial-track gesture token should fail before a no-op or mutation");
    const auto interruptedGesture = editor.beginOwnedHistoryTransaction();
    require(editor.applyPartialTrackEditBatch(
                source, batch, interruptedGesture).ok()
                && editor.setTimelineLoop({true, 0.0, 2.0}).ok()
                && !editor.applyPartialTrackEditBatch(
                    source, batch, interruptedGesture).ok(),
            "a sibling document edit should close and revoke the held partial-track gesture");
    editor.cancelOwnedHistoryTransaction(interruptedGesture);
    require(editor.document().timeline.loop.enabled
                && !editor.document().partialTrackEdits.empty(),
            "cancelling a revoked gesture must not roll back the later sibling edit or its accepted partial recipe");
    auto duplicateBatch = batch;
    duplicateBatch.pointMutations.push_back(
        duplicateBatch.pointMutations.front());
    require(!talas::project::applyProjectPartialTrackEditBatch(
                 source, nullptr, duplicateBatch).ok(),
            "one batch must not mutate the same stable point identity twice");

    auto invalid = edits;
    std::swap(invalid.pointOverrides[0], invalid.pointOverrides[1]);
    require(talas::project::validateProjectPartialTrackEditSet(invalid)
                .has_value(),
        "point overrides should require canonical source-identity order");
    invalid = edits;
    invalid.insertedPoints[0].editId = "voice_edits";
    invalid.insertedPoints.push_back(invalid.insertedPoints[0]);
    require(talas::project::validateProjectPartialTrackEditSet(invalid)
                .has_value(),
        "inserted point edit identities should be unique");
    invalid = edits;
    invalid.pointOverrides[0].source.trackId = 99;
    require(!talas::project::materializeProjectPartialTrackEdits(source, invalid)
                 .ok(),
        "missing source point identity should fail closed");
    invalid = edits;
    invalid.insertedPoints[0].sourceFrame = 256;
    invalid.insertedPoints[0].point.trackId = 1;
    require(!talas::project::materializeProjectPartialTrackEdits(source, invalid)
                 .ok(),
        "derived point collision should fail closed");
    invalid = edits;
    invalid.insertedPoints[0].point.frequencyHz = -1.0;
    require(talas::project::validateProjectPartialTrackEditSet(invalid)
                .has_value(),
        "edit recipes should reject invalid numeric point state before persistence");
    invalid = edits;
    invalid.trackStates.push_back({99, true, false});
    require(!talas::project::materializeProjectPartialTrackEdits(source, invalid)
                 .ok(),
        "mute/solo state for a missing track should fail closed");

    std::cout << "Project partial track edit tests passed\n";
    return 0;
}
