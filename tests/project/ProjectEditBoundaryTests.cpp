#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectEditBoundary.h"

#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument documentWithTitle(std::string title)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    return document;
}

void testCanonicalSnapshotTracksDirtyAndUndoToClean()
{
    const auto clean = documentWithTitle("Original");
    talas::project::ProjectEditBoundary state(clean);

    require(!state.isDirty(clean), "initial workspace document should be clean");
    require(!state.projectPath().has_value(), "initial workspace should be untitled");

    auto edited = clean;
    edited.metadata.title = "Edited";
    require(state.isDirty(edited), "changed document should be dirty");
    require(!state.isDirty(clean), "restoring the clean document should clear dirty state");
}

void testEditorUndoReturnsToCanonicalCleanState()
{
    const talas::project::ProjectDocument initial;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(initial, registry);
    talas::project::ProjectEditBoundary state(initial);

    const auto added = editor.addNode({"osc", "talas.oscillator", {}, {}});
    require(added.ok(), "workspace fixture should add a node");
    require(state.isDirty(editor.document()), "editor mutation should make the workspace dirty");

    const auto undone = editor.undo();
    require(undone.ok(), "workspace fixture should undo the node addition");
    require(!state.isDirty(editor.document()),
            "undoing to the canonical saved document should clear dirty state");

    const auto redone = editor.redo();
    require(redone.ok(), "workspace fixture should redo the node addition");
    require(state.isDirty(editor.document()), "redoing the edit should restore dirty state");
}

void testLoadedFormatElevenPresentationMoveMarksProjectDirty()
{
    talas::project::ProjectDocument source;
    source.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {},
        {},
    });
    talas::project::reconcileProjectPresentation(source);
    talas::project::findProjectNodePresentation(
        source,
        std::nullopt,
        "osc")->position = {24, 48};
    const auto loaded = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(source));
    require(loaded.ok(), "dirty-state format-eleven fixture should reopen");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{loaded.document, registry};
    talas::project::ProjectEditBoundary state(editor.document());
    require(!state.isDirty(editor.document()), "loaded format-eleven boundary should begin clean");
    require(
        editor.moveNode("osc", {320, -48}).ok(),
        "portable presentation move should succeed");
    require(
        state.isDirty(editor.document()),
        "portable presentation move should dirty the project after format-eleven load");
    require(editor.undo().ok(), "presentation move should remain undoable");
    require(
        !state.isDirty(editor.document()),
        "undoing the portable presentation move should restore the clean boundary");
}

void testSuccessfulSaveAndLoadAdvanceCleanBoundary()
{
    const auto initial = documentWithTitle("Initial");
    talas::project::ProjectEditBoundary state(initial);
    auto edited = documentWithTitle("Saved");
    const auto savePath = std::filesystem::path("/projects/saved.talas.json");

    require(state.isDirty(edited), "edited document should be dirty before save");
    state.markSaved(edited, savePath);
    require(!state.isDirty(edited), "successful save should establish a clean boundary");
    require(state.projectPath() == std::optional<std::filesystem::path>{savePath},
            "successful save should publish its project path");

    auto laterEdit = edited;
    laterEdit.metadata.title = "Unsaved";
    require(state.isDirty(laterEdit), "later edit should become dirty");
    require(state.isDirty(laterEdit),
            "a failed save that does not mark success must retain dirty state");
    require(state.projectPath() == std::optional<std::filesystem::path>{savePath},
            "a failed save must retain the prior project path");

    const auto loaded = documentWithTitle("Loaded");
    const auto loadedPath = std::filesystem::path("/projects/loaded.talas.json");
    state.markLoaded(loaded, loadedPath);
    require(!state.isDirty(loaded), "successful load should establish a clean boundary");
    require(state.projectPath() == std::optional<std::filesystem::path>{loadedPath},
            "successful load should publish its project path");
}

void testNewProjectClearsPathAndStartsClean()
{
    auto saved = documentWithTitle("Saved");
    talas::project::ProjectEditBoundary state(
        saved,
        std::filesystem::path("/projects/saved.talas.json"));
    auto dirty = saved;
    dirty.metadata.title = "Dirty";
    require(state.isDirty(dirty), "new-project fixture should begin dirty");

    const auto untitled = documentWithTitle("Untitled");
    state.markNew(untitled);
    require(!state.projectPath().has_value(), "new project should clear the file path");
    require(!state.isDirty(untitled), "new project should establish a clean boundary");

    auto edited = untitled;
    edited.metadata.title = "My Sound";
    require(state.isDirty(edited), "editing a new project should mark it dirty");
}

void testRecoveredProjectRetainsRecordedCleanBoundary()
{
    const auto clean = documentWithTitle("Last explicit save");
    auto recovered = clean;
    recovered.metadata.title = "Recovered edits";
    const auto path = std::filesystem::path("/projects/recovered.talas.json");

    talas::project::ProjectEditBoundary state(documentWithTitle("Startup demo"));
    state.markRecovered(clean, path);

    require(state.cleanDocument().metadata.title == "Last explicit save",
            "recovered workspace should retain the typed clean document");
    require(state.projectPath() == std::optional<std::filesystem::path>{path},
            "recovered workspace should retain a safe source path");
    require(state.isDirty(recovered),
            "recovered edits should remain dirty against the recorded clean boundary");
    require(!state.isDirty(clean),
            "returning to the recorded clean document should clear recovered dirty state");

    state.markRecovered(clean, {});
    require(!state.projectPath().has_value(),
            "unsafe recovered source identity should restore as untitled");
    require(state.isDirty(recovered),
            "restoring as untitled must not make recovered edits clean");
}

void testNativeUnsavedChangesButtonIndicesMapSafely()
{
    using talas::project::UnsavedChangesChoice;
    require(talas::project::unsavedChangesChoiceForButtonIndex(0) == UnsavedChangesChoice::Save,
            "first native dialog button should mean Save");
    require(talas::project::unsavedChangesChoiceForButtonIndex(1) == UnsavedChangesChoice::Discard,
            "second native dialog button should mean Discard");
    require(talas::project::unsavedChangesChoiceForButtonIndex(2) == UnsavedChangesChoice::Cancel,
            "third native dialog button should mean Cancel");
    require(talas::project::unsavedChangesChoiceForButtonIndex(-1) == UnsavedChangesChoice::Cancel
                && talas::project::unsavedChangesChoiceForButtonIndex(99)
                    == UnsavedChangesChoice::Cancel,
            "unexpected native dialog results should fail closed as Cancel");
}
}

int main()
{
    testCanonicalSnapshotTracksDirtyAndUndoToClean();
    testEditorUndoReturnsToCanonicalCleanState();
    testLoadedFormatElevenPresentationMoveMarksProjectDirty();
    testSuccessfulSaveAndLoadAdvanceCleanBoundary();
    testNewProjectClearsPathAndStartsClean();
    testRecoveredProjectRetainsRecordedCleanBoundary();
    testNativeUnsavedChangesButtonIndicesMapSafely();
    return 0;
}
