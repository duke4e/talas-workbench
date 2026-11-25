#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Compile to disk: a compiled Sound file holds a Sound ready to play by
// itself, without the project it came from: the closed program around it
// with the Sounds it uses, the global map, copies of the samples and MIDI
// files it reads, and what it was compiled for. Talas has no signal
// processor code to keep, so the file keeps the program Talas compiles
// as the file plays.
namespace talas::app
{
inline constexpr char compiledSoundExtension[] = ".talascompiled";

// What a Sound was compiled for: the project format, Talas's version
// and the operating system (recompile after updating the software or changing
// the system).
struct CompiledSoundTarget
{
    int projectFormat = 0;
    std::string application;
    std::string system;

    bool operator==(const CompiledSoundTarget&) const = default;
};

struct CompiledSound
{
    std::string soundDefinitionId;
    std::string name;
    CompiledSoundTarget target;
    talas::project::ProjectDocument document;
};

struct CompiledSoundResult
{
    std::optional<CompiledSound> compiled;
    std::string error;

    [[nodiscard]] bool ok() const
    {
        return compiled.has_value();
    }
};

// Compiles one of the document's Sounds (as a grid's box plays it, with the
// signal flow feeding its instance): the files it reads named by their full
// paths, each one's hash read from the disk.
[[nodiscard]] CompiledSoundResult compileSound(const talas::project::ProjectDocument& document,
    std::string_view soundDefinitionId, const std::optional<std::filesystem::path>& projectFile,
    CompiledSoundTarget target, std::optional<std::string_view> sourceInstanceId = {});

// The copies of its files a compiled Sound takes along, in a folder beside
// it named after it; each file's path in the compiled Sound becomes its
// copy's. Files of the same name are numbered.
struct CompiledSoundFileCopy
{
    std::filesystem::path from;
    std::filesystem::path to;

    bool operator==(const CompiledSoundFileCopy&) const = default;
};

[[nodiscard]] std::vector<CompiledSoundFileCopy> withFileCopies(
    CompiledSound& compiled, const std::filesystem::path& compiledFile);

[[nodiscard]] std::string writeCompiledSound(const CompiledSound& compiled);
[[nodiscard]] std::optional<CompiledSound> readCompiledSound(std::string_view text);

// Whether a compiled Sound still plays as its Sound would now: compiled for
// this Talas and system, and the same program, global map and files of
// the same contents, wherever their copies are (compiled Sounds do not follow
// changes to their Sounds, samples or MIDI files).
[[nodiscard]] bool compiledSoundCurrent(const CompiledSound& compiled, const CompiledSound& now);

// A grid's compiled Sounds, one file per box in the grid's own folder; and
// the boxes Compile to disk compiles: every box with a Sound for All
// Sounds, else those whose compiled Sound is missing or out of date
//.
[[nodiscard]] std::filesystem::path compiledGridSoundPath(const std::filesystem::path& folder,
    const talas::project::ProjectLiveSet& set, const talas::project::ProjectLiveSetCell& cell);
[[nodiscard]] std::vector<std::string> gridCellsToCompile(const talas::project::ProjectLiveSet& set,
    const std::map<std::string, bool>& currentByCell, bool allSounds);

// A Tool's Compiled Sound Grid: the project's Live
// Grid named as the grid's file is (without its extension, or by its id),
// else its only one; and the box playSoundNumber: names, counting from 1 at
// box 0, or playSoundNamed: names, by the box's name or its Sound's. The
// grid plays the box's compiled Sound, so it must be compiled first.
struct ToolGridBox
{
    const talas::project::ProjectLiveSet* set = nullptr;
    const talas::project::ProjectLiveSetCell* cell = nullptr;
};

[[nodiscard]] std::optional<ToolGridBox> toolGridBox(const talas::project::ProjectDocument& document,
    std::string_view gridFile, std::size_t number, std::string_view name);

// A Compiled Sound Grid's own file, its name ending in .prg: a Sound file holding the grid alone
// among the project's Sounds. Opened, the grid joins the project with the
// Sounds its boxes play, renamed where they would clash, and a grid id of
// its own; the id it took, or why it could not join.
inline constexpr char compiledSoundGridExtension[] = ".prg";
[[nodiscard]] std::filesystem::path compiledSoundGridFileName(std::string_view gridName);
[[nodiscard]] std::optional<talas::project::ProjectDocument> compiledSoundGridFile(
    const talas::project::ProjectDocument& project, std::string_view liveSetId);
struct CompiledSoundGridOpened
{
    std::optional<talas::project::ProjectDocument> document;
    std::string liveSetId;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept { return document.has_value(); }
};
[[nodiscard]] CompiledSoundGridOpened withCompiledSoundGridFile(const talas::project::ProjectDocument& project,
    const talas::project::ProjectDocument& gridFile, const talas::core::NodeRegistry& registry);
}
