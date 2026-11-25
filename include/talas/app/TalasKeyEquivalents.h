#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The key equivalents that the workbench's menus take as they are, with the
// Command key for Ctrl: Ctrl+K stops (kills) the Sound playing, Ctrl+] and
// Ctrl+[ zoom the active display in and out, and Ctrl+L opens the active field
// in a large window.
namespace talas::app
{
enum class TalasKeyCommand
{
    Stop,
    ZoomIn,
    ZoomOut,
    LargeWindow,
    FindPrototype,
    Find,
    FindAgain,
    Replace,
    Expand,
    MidiNotesOff,
    Close,
    Open,
    Save,
};

struct TalasKeyEquivalent
{
    TalasKeyCommand command;
    // The key held with Command (Ctrl).
    char key;
    std::string_view name;
    std::string_view menu;
    std::string_view description;
};

[[nodiscard]] std::span<const TalasKeyEquivalent> talasKeyEquivalents() noexcept;
[[nodiscard]] const TalasKeyEquivalent& talasKeyEquivalent(TalasKeyCommand command) noexcept;
// The command a key held with Command gives, if any.
[[nodiscard]] std::optional<TalasKeyCommand> talasKeyCommandFor(char key) noexcept;

// The transport's keys: Space plays and
// pauses, Ctrl+Space rewinds and plays every Sound again, Shift+Space plays
// the marked Sounds, Ctrl+Right Arrow plays, Ctrl+K stops. Ctrl+Space and
// Shift+Space keep the Control key (Command+Space is the system's search);
// Ctrl+Right Arrow and Ctrl+K take Command, as the menus' keys do (Control
// and the arrows switch the system's spaces).
enum class TalasTransportCommand
{
    PlayPause,
    RewindAndPlayAll,
    PlayMarked,
    Play,
    Stop,
};

enum class TalasTransportKeyName
{
    Space,
    RightArrow,
    K,
};

struct TalasTransportKey
{
    TalasTransportCommand command;
    TalasTransportKeyName key;
    bool control = false;
    bool commandKey = false;
    bool shift = false;
    std::string_view description;
};

[[nodiscard]] std::span<const TalasTransportKey> talasTransportKeys() noexcept;
[[nodiscard]] const TalasTransportKey& talasTransportKey(TalasTransportCommand command) noexcept;

// What Space, Ctrl+Space, Ctrl+R and Ctrl+K do in each state of the
// transport, in every window: after an edit or a new
// selection Space and Ctrl+Space compile and play, Ctrl+R replays the last
// Sound played; playing, Space pauses and Ctrl+R replays this Sound from its
// start; paused, Space resumes; stopped, Space and Ctrl+R replay the last
// Sound; Ctrl+Space always compiles and plays, Ctrl+K always stops.
enum class TalasTransportState
{
    Edited,
    Playing,
    Paused,
    Stopped,
};

enum class TalasTransportPress
{
    Space,
    ControlSpace,
    ControlR,
    ControlK,
};

enum class TalasTransportAction
{
    CompileAndPlay,
    Pause,
    Resume,
    ReplayLast,
    ReplayThis,
    Stop,
};

[[nodiscard]] TalasTransportAction talasTransportAction(TalasTransportState state, TalasTransportPress press) noexcept;

// Large window…: the active field's text in a
// window as large as the screen; Accept puts what the window holds back
// into the field, Cancel leaves the field as it was.
struct LargeWindowField
{
    std::string original;
    std::string edited;
};
[[nodiscard]] std::string largeWindowResult(const LargeWindowField& field, bool accepted);

// The menu bar: File, Edit, DSP, Action, Info and Tools, then View and Help.
enum class TalasMenu : std::uint8_t
{
    File,
    Edit,
    Dsp,
    Action,
    Info,
    Tools,
    View,
    Help,
};

inline constexpr std::size_t talasMenuCount = 8;

[[nodiscard]] std::string_view talasMenuName(TalasMenu menu) noexcept;
// What a menu is for: the files and windows; editing in the active
// window; configuring and monitoring the signal processor; acting on the
// selected Sounds; information on them; and the tools for designing sound.
[[nodiscard]] std::string_view talasMenuPurpose(TalasMenu menu) noexcept;

// The operations named by the menu they are in: the File menu's Virtual
// control surface; the DSP menu's Status, Stop, Restart, MIDI notes off,
// Configure MIDI, Initialize DSP and the MIDI and audio interfaces' settings;
// the Action menu's Compile, load, start (Play here, with Play Marked, Play
// All and Pause), Compile & load, Record to disk, Expand, Revert, Find
// prototype and Evaluate; and the Info menu's Describe Sound, Get info,
// Structure as text, Full waveform, Oscilloscope, Spectrum analyzer,
// Environment and Reset environment; the Edit menu's Preferences; and the
// Tools menu's Tape Recorder, the analysis tools, Synchronizing Spectra,
// Design Alternate Tunings, the File Archivist and the keyboard on the screen.
enum class TalasMenuOperation : std::uint8_t
{
    VirtualControlSurface,
    Status,
    Stop,
    Restart,
    MidiNotesOff,
    MidiMessages,
    ConfigureMidi,
    InitializeDsp,
    AudioSettings,
    CompileLoadStart,
    PlayMarked,
    PlayAll,
    Pause,
    CompileLoad,
    CompileToDisk,
    RecordToDisk,
    Expand,
    Revert,
    FindPrototype,
    Evaluate,
    NewClassFromExample,
    EditClass,
    RetrieveExampleFromClass,
    SetDefaultSound,
    SetDefaultCollection,
    DescribeSound,
    GetInfo,
    StructureAsText,
    FullWaveform,
    Oscilloscope,
    SpectrumAnalyzer,
    Environment,
    ResetEnvironment,
    Preferences,
    TapeRecorder,
    SpectralAnalysis,
    ReAnalysis,
    GaAnalysis,
    SynchronizingSpectra,
    DesignAlternateTunings,
    FileArchivist,
    OnScreenKeyboard,
};

[[nodiscard]] TalasMenu talasMenuOf(TalasMenuOperation operation) noexcept;

// Where an operation applies: Revert to the Sound
// selected in the Sound editor (here the Patch workspace's signal flow and
// parameters), Environment in the Sound editor, Find prototype where the
// prototypes are shown; the menus dim the operations that do not apply.
// New class from example only in a Sound file window, not in a Sound editor or a Sound Browser.
struct TalasMenuContext
{
    bool soundEditorShown = true;
    bool soundSelected = false;
    bool prototypesShown = true;
    bool soundFileWindowInFront = false;
};

[[nodiscard]] bool talasMenuOperationApplies(TalasMenuOperation operation, const TalasMenuContext& context) noexcept;
// What to do for a dimmed operation to apply, empty for one that needs
// nothing more than the menus' own selection.
[[nodiscard]] std::string_view talasMenuOperationNeeds(TalasMenuOperation operation) noexcept;

// A click with Shift held stops a recording
// to disk, or a Sound playing from the disk, even when the other controls
// wait for the disk; any other click is left alone.
[[nodiscard]] bool shiftClickStopsDiskOperation(bool recordingToDisk, bool diskSoundPlaying, bool shiftHeld) noexcept;
// While a Sound plays from or records to the disk,
// Ctrl+K and the animated displays (DSP Status) work only with Update
// display during disk operations on; Shift+click stops it either way.
[[nodiscard]] bool interfaceRunsDuringDiskOperation(bool diskSoundPlaying, bool updateDisplayDuringDiskOperations) noexcept;

// The cursor shows what Talas is busy with: a
// watch while a Sound compiles, a hollow arrow while it plays; while it
// uses the disk, an arrow partly filled (a black dot inside) with Update
// display during disk operations on, a floppy disk with it off (Shift+click
// stops the Sound then); the plain, filled arrow once it has finished or
// been stopped.
enum class WorkbenchCursor : std::uint8_t
{
    Arrow,
    Watch,
    Playing,
    PlayingFromDisk,
    WaitingForDisk,
    // Eyeglasses while a file is looked for.
    Locating,
    // An animated icon while samples load.
    LoadingSamples,
    // A pen while an analysis is written to its file.
    WritingFile,
};
struct WorkbenchActivity
{
    bool locatingFile = false;
    bool compiling = false;
    bool playing = false;
    bool usingDisk = false;
    bool updateDisplayDuringDiskOperations = true;
    bool loadingSamples = false;
    bool writingFile = false;
};
[[nodiscard]] WorkbenchCursor workbenchCursor(const WorkbenchActivity& activity) noexcept;

// When the signal processor, here the audio device, stops
// answering, a dialog says so and that Initialize DSP from the DSP menu
// starts it again.
[[nodiscard]] std::string dspProblemMessage(std::string_view reason);

// Expand replacing a Sound in the signal flow with its
// expansion warns that the original cannot be recovered, Expand or Cancel.
[[nodiscard]] std::string expandReplacementWarning();

// In a parameter field the Space Bar types a space,
// and Ctrl+Space plays the Sound being edited; nothing else goes to the
// transport.
[[nodiscard]] std::optional<TalasTransportPress> parameterFieldTransportPress(bool space, bool control) noexcept;

// The Edit menu's Cut, Copy, Paste and Select All: while a text field is
// active, in the Sound editor or in a window of its own as the class editor's
// description and comments are, they edit its text; else they act on the
// selected Sounds.
enum class EditMenuTarget : std::uint8_t
{
    TextField,
    Sounds,
};
[[nodiscard]] EditMenuTarget editMenuTarget(bool inSoundEditor, bool inToolWindow) noexcept;

// Quit closes the windows, asking to save each whose
// contents changed (the sample editor's sample, a new class in the class
// editor, the project), then saves the preferences and exits. Closing the
// Sound editor while a parameter field holds a problem first asks whether to
// drop the changes or continue editing.
enum class QuitStep : std::uint8_t
{
    AskIllegalValue,
    AskSampleEditor,
    AskClassEditor,
    AskProject,
    SavePreferences,
    Exit,
};
struct QuitState
{
    bool sampleChanged = false;
    bool newClassOpen = false;
    bool projectChanged = false;
    bool illegalValueHeld = false;
};
[[nodiscard]] std::vector<QuitStep> quitSteps(const QuitState& state);
}
