#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectGraphClipboard.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Sound file window: the project's Sounds as icons, a workspace for making and
// editing them. A click selects a Sound, Shift+Click adds it to the
// selection or takes it out, and a box drawn in the background selects the
// Sounds within it, with Shift added to or taken from the selection.
namespace talas::app
{
struct SoundFileIcon
{
    // A Sound's definition id, or a Sound collection's id.
    std::string definitionId;
    std::string name;
    bool collection = false;
    // The Sound open in the Sound editor, its icon patterned grey.
    bool beingEdited = false;

    bool operator==(const SoundFileIcon&) const = default;
};

// The Sounds of the project, in their order, or (format 140) those of one
// level: the Sound collections there, then the Sounds there, at the top level
// those in no collection.
// editing: the Sound open in the Sound editor, if any.
[[nodiscard]] std::vector<SoundFileIcon> soundFileIcons(
    const talas::project::ProjectDocument& document, const std::optional<std::string>& collectionId = std::nullopt,
    const std::optional<std::string>& editing = std::nullopt);
// The collection holding a collection, if any: the level above it.
[[nodiscard]] std::optional<std::string> soundFileParentCollection(
    const talas::project::ProjectDocument& document, std::string_view collectionId);

// A double-click on an icon: a collection opens to show its level, a Sound opens in
// a Sound editor, its signal flow above and its parameters below. A Sound
// already being edited is not opened again: its Sound editor comes to the
// front as it was, its signal flow, selection and the Sound whose
// parameters are open kept.
enum class SoundFileDoubleClick : std::uint8_t
{
    OpenCollection,
    EditSound,
    BringEditorToFront,
};

[[nodiscard]] SoundFileDoubleClick soundFileDoubleClick(const SoundFileIcon& icon) noexcept;

// The signal flow diagram a Sound editor shows: that of the Sound being
// edited, its own Sounds and lines, or the project's own signal flow when it
// edits none; nothing when the project has no such Sound.
[[nodiscard]] const talas::project::ProjectGraph* soundEditorSignalFlow(
    const talas::project::ProjectDocument& document, const std::optional<std::string>& editedDefinitionId) noexcept;

// The selection after a click on a Sound, with Shift or not; a click on the
// background (an empty id) without Shift selects nothing.
[[nodiscard]] std::vector<std::string> soundFileClick(
    std::vector<std::string> selection, std::string_view definitionId, bool shift);

// The selection after a box drawn round `within`: those Sounds, or with
// Shift each one added to the selection or taken out of it.
[[nodiscard]] std::vector<std::string> soundFileBoxSelection(
    std::vector<std::string> selection, const std::vector<std::string>& within, bool shift);

// The variables a
// play asks values for. The Sound editor keeps an environment, so it asks
// only those it has no value for; the Sound file window keeps none and asks
// them all, each time it plays the Sound.
enum class VariablePlayOrigin : std::uint8_t
{
    SoundEditor,
    SoundFileWindow,
};
[[nodiscard]] std::vector<std::string> variablesToAskFor(const talas::project::ProjectDocument& document,
    const std::vector<std::pair<std::string, double>>& environment, VariablePlayOrigin origin);

// The Sounds dragged with Control or Option held
// move, rather than a copy of them: the project with those Sounds, in their
// order, just before the Sound `before`, or last where it is none of the
// others; nothing when none of them is the project's.
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundFileWindowMoved(
    talas::project::ProjectDocument document, const std::vector<std::string>& definitionIds, std::string_view before);

// Where the icons sit: as many columns of `cell` points as fit the width,
// at least one, row after row.
struct SoundFileCell
{
    std::size_t column = 0;
    std::size_t row = 0;

    bool operator==(const SoundFileCell&) const = default;
};

// A prototype dragged into the Sound file window:
// a new Sound of the project, the prototype in its signal flow with noise
// in its required audio inputs and its audio outputs the Sound's, named for
// the prototype with the first number its name lacks appended; nothing for
// an unknown prototype, one with no audio output (except one that plays
// without it, as a SoundToGlobalController does), or one requiring an input of
// another kind. Given the numbers the prototypes last gave their copies, kept
// with the preferences, the number goes on from the prototype's, and is kept
// there.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> soundForPrototype(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry, std::string_view typeId,
    std::map<std::string, unsigned>* copyNumbers = nullptr);

// A Sound file opened as a custom prototype strip: each Sound collection a
// category of its Sounds, in the file's order, and the Sounds in no collection
// under General, last.
struct CustomPrototype
{
    std::string definitionId;
    std::string name;

    bool operator==(const CustomPrototype&) const = default;
};

struct CustomPrototypeCategory
{
    std::string name;
    std::vector<CustomPrototype> sounds;

    bool operator==(const CustomPrototypeCategory&) const = default;
};

[[nodiscard]] std::vector<CustomPrototypeCategory> customPrototypeStrip(const talas::project::ProjectDocument& soundFile);

// Several Sounds or collections played together from the Sound file window
//: one after another, in the order given (top to
// bottom and left to right as the icons lie), a collection's Sounds in its
// own order, each once: a Sound of a Concatenation of them, under an id no
// Sound has; nothing for fewer than two Sounds.
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> soundsInTurn(
    const talas::project::ProjectDocument& document, const std::vector<std::string>& selected);
// The selection in the order its icons lie.
[[nodiscard]] std::vector<std::string> soundFileSelectionInOrder(
    const std::vector<SoundFileIcon>& icons, const std::vector<std::string>& selection);

// A Sound dragged out of a signal flow: what the drag carries, the signal flow (none for
// the project's) and the Sound; and the copy it leaves in a Sound file
// window, the Sound with every Sound feeding it laid out as they are, its
// audio outputs the copy's, named for it (with the first number free when
// the name is taken). Nothing for a Sound without an audio output.
struct SignalFlowSoundDrag
{
    std::optional<std::string> scope;
    std::string nodeId;

    bool operator==(const SignalFlowSoundDrag&) const = default;
};

struct SignalFlowSoundCopy
{
    talas::project::ProjectSoundDefinition definition;
    talas::project::ProjectStructureLayout layout;
};

[[nodiscard]] std::string signalFlowSoundDragDescription(const std::optional<std::string>& scope, std::string_view nodeId);
[[nodiscard]] std::optional<SignalFlowSoundDrag> signalFlowSoundDrag(std::string_view description);
[[nodiscard]] std::optional<SignalFlowSoundCopy> soundCopiedFromSignalFlow(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag);
// A Sound of a signal flow dragged into a parameter field: the field takes
// that Sound's own value for its parameter of the same name, an expression's
// text, its prototype's when the Sound never set it; nothing when the Sound
// has no expression parameter so named.
// Sounds copied in a signal flow and pasted where Sounds go, a Timeline's
// track: the copy's last Sound, the one feeding no
// other copied, with the Sounds feeding it, a Sound of its own as one
// dragged out is; nothing for a copy without such a Sound.
[[nodiscard]] std::optional<SignalFlowSoundCopy> soundCopiedFromFragment(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const talas::project::ProjectGraphFragment& fragment);
// Copy (Ctrl+C) in the Sound file window or the Prototypes: the Sounds
// selected, each as a Sound of the signal flow playing it, or the prototype as
// a Sound of its own, for Paste to put into a signal flow or a Sound file
// window.
[[nodiscard]] talas::project::ProjectGraphFragment soundFileWindowCopied(const std::vector<std::string>& definitionIds);
[[nodiscard]] std::optional<talas::project::ProjectGraphFragment> prototypeCopied(
    const talas::core::NodeRegistry& registry, std::string_view typeId);
// The Sounds of a project a copy names, each once, when every Sound copied
// is one; Paste into the Sound file window duplicates them.
[[nodiscard]] std::vector<std::string> soundsOfCopy(
    const talas::project::ProjectDocument& document, const talas::project::ProjectGraphFragment& fragment);
// A Sound of a signal flow dropped with Control or Option held on another
// Sound of the same signal flow: the Sound itself, not a copy, takes the other's
// place, feeding what the other fed, so that it is shared; the other leaves
// the signal flow with the Sounds that fed only it. Nothing when the other
// feeds the Sound (a loop), is the Sound, or names an input of its Sound,
// or when the Sound has no output.
[[nodiscard]] std::optional<talas::project::ProjectDocument> soundSharedInPlaceOf(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag, std::string_view replacedId);
[[nodiscard]] std::optional<std::string> parameterValueOfDraggedSound(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag, std::string_view parameterId);

// Removing Sounds asks before each one,
// Yes removing it and asking about the next, Remove all (offered while more
// than one is left) removing it and the rest, Cancel keeping it and the rest.
enum class SoundRemovalAnswer
{
    Yes,
    RemoveAll,
    Cancel,
};

struct SoundRemoval
{
    std::vector<std::string> removed;
    // Those still to ask about; the first is asked next.
    std::vector<std::string> pending;

    bool operator==(const SoundRemoval&) const = default;
};

[[nodiscard]] std::vector<std::string> soundRemovalChoices(std::size_t pending);
// The question asked: a Sound deleted alone that has
// inputs is replaced by its input, so the question asks about that rather
// than about removing it.
[[nodiscard]] std::string soundRemovalQuestion(std::string_view name, const std::vector<std::string>& inputs);
[[nodiscard]] std::optional<SoundRemovalAnswer> soundRemovalAnswer(std::string_view choice) noexcept;
[[nodiscard]] SoundRemoval soundRemovalAnswered(SoundRemoval removal, SoundRemovalAnswer answer);

[[nodiscard]] std::size_t soundFileColumns(int width, int cell) noexcept;
[[nodiscard]] SoundFileCell soundFileCell(std::size_t index, std::size_t columns) noexcept;

// Sound editors: any number may be open at once,
// each on its own Sound, but only one on any one Sound. Opening a Sound
// whose editor is open brings that editor to the front rather than opening
// a second one. The editors keep the order they were opened in (the tabs
// the Workbench shows them as); none is in front while the project's own
// signal flow shows, the others staying open behind.
struct OpenSoundEditor
{
    // The Sound it was opened on.
    std::string definitionId;
    // Where it has gone since: that Sound, then each of its Sounds opened
    // in turn; the editor shows the last.
    std::vector<std::string> path;
    // The Sound as the editor opened on it, which the
    // close box's Drop gives back.
    std::optional<talas::project::ProjectSoundDefinition> opened;

    bool operator==(const OpenSoundEditor&) const = default;
};

struct SoundEditorWindows
{
    std::vector<OpenSoundEditor> editors;
    std::optional<std::string> front;

    bool operator==(const SoundEditorWindows&) const = default;
};

enum class SoundEditorOpening : std::uint8_t
{
    Opened,
    BroughtToFront,
    // No Sound of the project has that id.
    Refused,
};

// An editor opened on a Sound of the project, in front; or, when one is
// open on it already, that one brought to the front as it was.
SoundEditorOpening openSoundEditor(
    SoundEditorWindows& windows, const talas::project::ProjectDocument& document, std::string_view definitionId);
[[nodiscard]] const OpenSoundEditor* soundEditorOn(const SoundEditorWindows& windows, std::string_view definitionId) noexcept;
[[nodiscard]] const OpenSoundEditor* frontSoundEditor(const SoundEditorWindows& windows) noexcept;
// The editor on a Sound closed; after the one in front, none is.
void closeSoundEditor(SoundEditorWindows& windows, std::string_view definitionId);
// What the Workbench's Sound editor pane shows now, the path from the Sound
// opened to the one shown: the editor on its first Sound in front (opened
// when none is) and keeping that path; an empty path puts none in front.
void followSoundEditorPath(
    SoundEditorWindows& windows, const talas::project::ProjectDocument& document, const std::vector<std::string>& path);
// The editors on Sounds the project no longer has close, and a path stops
// before the first Sound it no longer has.
void reconcileSoundEditors(SoundEditorWindows& windows, const talas::project::ProjectDocument& document);
// Another document in place, as when a Sound file is opened: the editors
// on the old one close, except the one in front when the new one has its
// Sound, which stays as if opened now.
void soundEditorsOnNewDocument(SoundEditorWindows& windows, const talas::project::ProjectDocument& document);
// Saved: each editor's Sound as it is now is what Drop gives
// back.
void soundEditorsSaved(SoundEditorWindows& windows, const talas::project::ProjectDocument& document);
// The Sounds with an editor open, in the editors' order; the Sound file
// window marks each as being edited.
[[nodiscard]] std::vector<std::string> soundsBeingEdited(const SoundEditorWindows& windows);
[[nodiscard]] std::vector<SoundFileIcon> soundFileIcons(const talas::project::ProjectDocument& document,
    const std::optional<std::string>& collectionId, const std::vector<std::string>& editing);
// A Sound editor's title, its Sound's name.
[[nodiscard]] std::string soundEditorTitle(const talas::project::ProjectDocument& document, const OpenSoundEditor& editor);

// The window an edit is kept with, for Undo: the
// Workbench's own window, where the Sound editors (their tabs) and the
// project's signal flow are, or another window, such as the Sound file
// window, by its name. Never 0, and the same window always gives the same
// number.
[[nodiscard]] std::uint64_t soundEditorHistoryWindow() noexcept;
[[nodiscard]] std::uint64_t namedHistoryWindow(std::string_view windowName) noexcept;

// Renaming a SoundCollectionVariable: dragged out
// of an Inputs field into the Sound file window, it is a Sound of its own
// there, the variable alone (soundCopiedFromSignalFlow). Enter renames
// that Sound and with it the variable it stands for; dragged back into an
// Inputs field it goes in as the variable itself, under its new name. A
// Variable renames the same way.
// The variable a Sound of the project is, alone: its one node, a
// SoundCollectionVariable or a Variable; nothing for another Sound.
[[nodiscard]] const talas::project::ProjectNode* soundFileVariable(
    const talas::project::ProjectDocument& document, std::string_view definitionId);
// That Sound renamed, its variable named as it is: nothing when the Sound
// is not a variable alone or the name is not a variable's name (a letter,
// then letters and digits).
[[nodiscard]] std::optional<talas::project::ProjectSoundDefinition> soundFileVariableRenamed(
    const talas::project::ProjectDocument& document, std::string_view definitionId, std::string_view name);
// A Sound of the Sound file window dropped into an input field of a signal
// flow: a variable alone goes in as itself, named for the variable (with
// the first number free in that signal flow when the name is taken); any
// other Sound as a Sound of it. Nothing for a Sound the project lacks.
[[nodiscard]] std::optional<talas::project::ProjectNode> soundFileSoundForInput(const talas::project::ProjectDocument& document,
    std::optional<std::string_view> scope, std::string_view definitionId);
}
