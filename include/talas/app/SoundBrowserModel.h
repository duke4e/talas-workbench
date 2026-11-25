#pragma once

#include "talas/app/ExpressionsLibraryModel.h"
#include "talas/app/TalasFiles.h"
#include "talas/project/ProjectDocument.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The Sound Browser: the folders on disk, the
// Sound files (projects) and the Sounds inside them, samples, spectra,
// analyses and MIDI files, each kind in its colour; the coloured boxes that
// show or hide a kind; the information on the selected row; finding a name;
// and the replacement input substituted for the input of a Sound that has
// one.
namespace talas::app
{
enum class SoundBrowserKind : std::uint8_t
{
    Folder,
    SoundFile,
    Sound,
    Sample,
    Spectrum,
    Ga,
    Re,
    Midi,
    // A text file of expressions, and
    // each expression in it.
    ExpressionsFile,
    Expression,
};

inline constexpr std::size_t soundBrowserKindCount = 10;

[[nodiscard]] std::optional<SoundBrowserKind> soundBrowserKindOf(const std::filesystem::path& path);

// A double-click on a row:
// a folder spawns a browser of it, a Sound file opens or closes, a Sound
// comes into the project to be edited, a sample or a spectrum opens in its
// editor, and any other file plays.
enum class SoundBrowserActivation : std::uint8_t
{
    Spawn,
    Toggle,
    // A Sound file's name double-clicked opens a Sound
    // file window of its Sounds as icons; its triangle opens it in place.
    OpenSoundFileWindow,
    Edit,
    OpenInEditor,
    Play,
    // An expression is dragged into a field, not opened.
    Nothing,
};

[[nodiscard]] SoundBrowserActivation soundBrowserActivation(SoundBrowserKind kind) noexcept;

// Folders black, Sound files and Sounds blue, the rest in the File
// Organizer's colours; as ARGB. A Timeline's file (.talastimeline), a Sound file
// holding a Timeline, is olive.
inline constexpr std::uint32_t soundBrowserTimelineColour = 0xffa3a03cu;
[[nodiscard]] std::uint32_t soundBrowserKindColour(SoundBrowserKind kind) noexcept;
[[nodiscard]] std::string_view soundBrowserKindName(SoundBrowserKind kind) noexcept;

struct SoundBrowserRow
{
    std::filesystem::path path;
    // A Sound's definition in its Sound file; empty for files and folders.
    std::string definitionId;
    std::string name;
    std::size_t depth = 0;
    SoundBrowserKind kind = SoundBrowserKind::Folder;
    bool open = false;
    // A Sound with an input to replace, shown with a cyan arrow.
    bool processesInput = false;

    bool operator==(const SoundBrowserRow&) const = default;
};

// How the browser draws a row: a
// folder's name in the text colour, a Sound file's and a Sound's in their
// blues, a Sound file's underlined; a closed folder or Sound file faded
// beside a sideways triangle, an open one saturated beside a downward one;
// an arrow beside a Sound that processes its input.
enum class SoundBrowserTriangle
{
    None,
    Sideways,
    Down,
};

struct SoundBrowserRowLook
{
    // None for the text colour (a folder's); ARGB otherwise.
    std::optional<std::uint32_t> colour;
    bool faded = false;
    SoundBrowserTriangle triangle = SoundBrowserTriangle::None;
    bool underlined = false;
    bool arrow = false;

    bool operator==(const SoundBrowserRowLook&) const = default;
};

[[nodiscard]] SoundBrowserRowLook soundBrowserRowLook(const SoundBrowserRow& row) noexcept;

// A row dragged out of a Sound Browser: a
// Sound as its Sound file and its definition there, a sample, analysis or
// MIDI file as the file itself, as the File Organizer drags one; folders and
// Sound files are not dragged. Either becomes a Sound of the project where
// it is dropped.
struct SoundBrowserDrag
{
    std::filesystem::path path;
    // The Sound's definition; empty for a file.
    std::string definitionId;

    bool operator==(const SoundBrowserDrag&) const = default;
};

[[nodiscard]] std::string soundBrowserDragDescription(const SoundBrowserRow& row);
[[nodiscard]] std::optional<SoundBrowserDrag> soundBrowserDrag(std::string_view description);

// A Sound file's Sounds, as the browser lists them.
struct SoundBrowserSound
{
    std::string definitionId;
    std::string name;
    bool processesInput = false;

    bool operator==(const SoundBrowserSound&) const = default;
};

[[nodiscard]] std::vector<SoundBrowserSound> soundBrowserSounds(const talas::project::ProjectDocument& document);

// A browser's title: Sound Browser for the main
// one, the folder's name for one spawned on a folder by a double-click.
[[nodiscard]] std::string soundBrowserTitle(const std::optional<std::filesystem::path>& folder);

class SoundBrowserModel
{
public:
    void setRoots(std::vector<std::filesystem::path> roots);
    [[nodiscard]] const std::vector<std::filesystem::path>& roots() const noexcept;
    // The triangle beside a folder or Sound file opens or closes it.
    void toggle(const std::filesystem::path& path);
    [[nodiscard]] bool isOpen(const std::filesystem::path& path) const;

    // The coloured boxes: a click shows that kind only, Shift+Click
    // adds or removes it, the leftmost grey box shows every kind. Folders
    // always show.
    void showOnly(SoundBrowserKind kind);
    void toggleKind(SoundBrowserKind kind);
    void showAll();
    [[nodiscard]] bool shows(SoundBrowserKind kind) const noexcept;

    // The list: each root and, beneath the open folders and Sound files and
    // indented by depth, their folders, the files of the kinds shown and a
    // Sound file's Sounds, folders first and each by name.
    [[nodiscard]] std::vector<SoundBrowserRow> rows(std::size_t limit = 4096) const;

    // Find: the first row after the one given whose name holds the
    // text, ignoring case, wrapping round from the end of the list to the
    // top, among the open folders and then, with none there, every folder
    // beneath the roots, of the kinds shown only; the folders and Sound file
    // holding it open. Nothing when no other row matches. A search stops,
    // finding and opening nothing, once `stopped` answers true, which it is
    // asked between the folders and files it reads (a click or a key stops
    // it).
    [[nodiscard]] std::optional<SoundBrowserRow> find(std::string_view text, const std::optional<SoundBrowserRow>& after,
        const std::function<bool()>& stopped = {});

    // The Refresh button: Sound files read again.
    void refresh();

private:
    [[nodiscard]] const std::vector<SoundBrowserSound>& soundsOf(const std::filesystem::path& soundFile) const;
    void walk(const std::filesystem::path& folder, std::size_t depth, bool openOnly, std::size_t limit,
        std::vector<SoundBrowserRow>& rows, const std::function<bool()>& stopped = {}) const;

    std::vector<std::filesystem::path> roots_;
    std::set<std::filesystem::path> open_;
    std::array<bool, soundBrowserKindCount> shown_{true, true, true, true, true, true, true, true, true, true};
    mutable std::map<std::filesystem::path, std::vector<SoundBrowserSound>> sounds_;
};

// The information at the bottom of the browser: for a Sound its
// kind, name, description and MIDI when it expects MIDI input; for
// a sample its pitch, samples, rate, bit depth and channels; for an
// expression its comment, then its code; for other files
// what the File Organizer shows.
[[nodiscard]] std::vector<std::string> soundBrowserInformation(const SoundBrowserRow& row);

// The expression an Expression row names: the
// expressions file it is in and its place there.
[[nodiscard]] std::optional<ExpressionLibraryEntry> soundBrowserExpression(const SoundBrowserRow& row);

// A Sound that responds to MIDI keys: a MIDIVoice, a key-reading Event
// Value (!KeyDown, !KeyPitch, !KeyVelocity, !KeyNumber, !KeyTimbre) or one
// of its Sounds that does.
[[nodiscard]] bool soundExpectsMidi(const talas::project::ProjectDocument& document, std::string_view definitionId);

// A sample's pitch from its header, as a MIDI note number; middle C, 60,
// for one that gives none.
[[nodiscard]] unsigned samplePitchNoteNumber(const std::filesystem::path& path);

// Short samples loop when played in the browser: under 30 seconds,
// unless Play is clicked with Control held, which plays the
// sample once.
[[nodiscard]] bool soundBrowserLoops(double seconds, bool once = false) noexcept;

// Each kind of file plays in the browser: a sample
// (or GA wavetable) as itself, a Sound as itself, a spectrum, RE analysis
// or MIDI file through the Sound that plays it, and a Sound file holding a
// Timeline as that Timeline.
enum class SoundBrowserPlayer : std::uint8_t
{
    Sample,
    Sound,
    FileSound,
    Timeline,
    None,
};

[[nodiscard]] SoundBrowserPlayer soundBrowserPlayer(SoundBrowserKind kind) noexcept;

enum class SoundBrowserInputKind : std::uint8_t
{
    AudioInput,
    Sample,
    Sound,
};

// The replacement input: an audio input channel, a sample or
// another Sound; the connection button engages or disengages it; one for
// every browser.
struct SoundBrowserReplacementInput
{
    SoundBrowserInputKind kind = SoundBrowserInputKind::AudioInput;
    unsigned audioInput = 1;
    std::filesystem::path path;
    std::string definitionId;
    std::string name;
    bool connected = false;

    bool operator==(const SoundBrowserReplacementInput&) const = default;
};

// The text the field shows: "Input 1", a sample's name or a Sound's.
[[nodiscard]] std::string soundBrowserInputText(const SoundBrowserReplacementInput& input);
// The audio input button: the next input, back to Input 1 after the last.
[[nodiscard]] SoundBrowserReplacementInput nextAudioInput(SoundBrowserReplacementInput input, unsigned inputCount);
// The substitute input button: the selected sample or Sound, connected.
[[nodiscard]] std::optional<SoundBrowserReplacementInput> substituteInput(const SoundBrowserRow& row);
}
