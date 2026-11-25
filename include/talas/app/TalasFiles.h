#pragma once

#include "talas/app/AnalysisToolsModel.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/SpeakerPlacement.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The files Talas works with: their
// kinds and colours, the File Organizer's folders, a file's information,
// how a file opens or becomes a Sound, and the preferences kept in a file
// of their own.
namespace talas::app
{
enum class TalasFileKind : std::uint8_t
{
    Sample,
    Spectrum,
    Ga,
    Re,
    Midi,
};

// The kind a file's name gives: a GA analysis ends " GA.wav", an RE table
// " RE.txt", a spectrum .talaspartial or .spc, a MIDI file .mid or .midi, a
// sample (an EX excitation among them) .wav, .aif, .aiff, .sf, .sd1 or
// .sd2; nothing for other files.
[[nodiscard]] std::optional<TalasFileKind> talasFileKindOf(const std::filesystem::path& path);

// The organizer's colour for each kind: samples turquoise,
// spectra purple, GA red-brown, RE yellow-orange, MIDI green; as ARGB.
[[nodiscard]] std::string_view talasFileKindColourName(TalasFileKind kind) noexcept;
[[nodiscard]] std::uint32_t talasFileKindColour(TalasFileKind kind) noexcept;

// How the organizer shows a disk or folder: gray while it
// is closed, underlined black once it is open; as ARGB.
struct FileOrganizerFolderLook
{
    std::uint32_t colour = 0;
    bool underlined = false;
};
[[nodiscard]] FileOrganizerFolderLook fileOrganizerFolderLook(bool open) noexcept;

struct FileOrganizerRow
{
    std::filesystem::path path;
    std::size_t depth = 0;
    bool folder = false;
    bool open = false;
    std::optional<TalasFileKind> kind;

    bool operator==(const FileOrganizerRow&) const = default;
};

// The organizer's list: each root folder and, beneath the open ones and
// indented by their depth, their folders and the files Talas can use, folders
// first and each by name. A double-click opens or closes a folder.
class FileOrganizerModel
{
public:
    void setRoots(std::vector<std::filesystem::path> roots);
    void toggle(const std::filesystem::path& folder);
    [[nodiscard]] bool isOpen(const std::filesystem::path& folder) const;
    [[nodiscard]] std::vector<FileOrganizerRow> rows(std::size_t limit = 4096) const;
    // Edit > View...: folders before the files, or among
    // them by name.
    void setFoldersFirst(bool foldersFirst) noexcept { foldersFirst_ = foldersFirst; }
    [[nodiscard]] bool foldersFirst() const noexcept { return foldersFirst_; }

private:
    std::vector<std::filesystem::path> roots_;
    std::set<std::filesystem::path> open_;
    bool foldersFirst_ = true;
};

// Edit > View...: what it sets for the window in
// front, the size of a Sound file window's icons, a Sound editor's icon size
// and connection style and where its parameters are (beside the signal flow,
// or below it), or how the file organizer shows folders.
enum class ViewedWindow
{
    SoundFileWindow,
    SoundEditor,
    FileOrganizer,
};

struct ViewChoice
{
    std::string group;
    std::string name;

    bool operator==(const ViewChoice&) const = default;
};

[[nodiscard]] std::vector<ViewChoice> viewChoices(ViewedWindow window);
// The signal flow's scale for large or small icons: small
// icons take a quarter of the area of large ones.
[[nodiscard]] double signalFlowIconScale(bool large) noexcept;
// Icon Size in Sound Editor Fields: an input field shows its Sounds as icons, small or
// large, each with its name below, left to right and top to bottom in their
// order, as many to a row as the field's width holds: the
// icon's side, and each Sound's place in the field.
struct SoundFieldIconPlace
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool operator==(const SoundFieldIconPlace&) const = default;
};
[[nodiscard]] int soundFieldIconSize(bool large) noexcept;
[[nodiscard]] std::vector<SoundFieldIconPlace> soundFieldIconPlaces(std::size_t count, int width, bool large);

// What the organizer shows of a file: its full path, when it was last
// modified, and what its kind's header holds (a sample's channels, rate and
// length, a spectrum's tracks and frames, a MIDI file's length and
// channels).
[[nodiscard]] std::vector<std::string> talasFileInformation(const std::filesystem::path& path);

// Spectrum Analyzer:
// the FFT length and window of the spectrum analyzer the Info menu opens.
// A SpectrumAnalyzerDisplay Sound of one's own is not affected: it keeps the
// analysis panel's own settings. Besides Rectangular, Hann and Blackman, the
// windows LinearEnvelope, Hamming and Long Kaiser.
enum class SpectrumAnalyzerWindow : std::uint8_t
{
    Rectangular,
    Hann,
    Blackman,
    LinearEnvelope,
    Hamming,
    LongKaiser,
};
// The windows in the order the Preferences offer them, with the names the
// preferences file keeps (`linearEnvelope`, ...) and those shown.
[[nodiscard]] const std::vector<SpectrumAnalyzerWindow>& spectrumAnalyzerWindows();
[[nodiscard]] std::string_view spectrumAnalyzerWindowKey(SpectrumAnalyzerWindow window) noexcept;
[[nodiscard]] std::string_view spectrumAnalyzerWindowLabel(SpectrumAnalyzerWindow window) noexcept;
[[nodiscard]] std::optional<SpectrumAnalyzerWindow> spectrumAnalyzerWindowFromKey(std::string_view key) noexcept;
struct SpectrumAnalyzerSettings
{
    unsigned length = 256;
    SpectrumAnalyzerWindow window = SpectrumAnalyzerWindow::Hann;

    bool operator==(const SpectrumAnalyzerSettings&) const = default;
};
[[nodiscard]] SpectrumAnalyzerSettings spectrumDisplaySettings(
    bool infoAnalyzer, const SpectrumAnalyzerSettings& preferences, const SpectrumAnalyzerSettings& panel) noexcept;

// The preferences, kept in a file of their own
// for each user and read at every start; a missing or unreadable file, or a
// line it does not know, leaves the defaults.
struct WorkbenchPreferences
{
    // Appearance: file names with their whole paths; numbers with all
    // twelve digits rather than six.
    bool showFullPathNames = false;
    bool showAllDigits = false;
    // Icon Size in Sound Editor Fields: large icons for the Sounds in Input
    // and other Sound fields, else small ones.
    bool largeFieldIcons = false;
    // Show Eight Channels in DSP Status: eight input
    // and output meters rather than two.
    bool showEightChannels = false;
    // Performance: the virtual control surface opens as a Sound with Event
    // Values plays.
    bool openVirtualControlsOnPlay = true;
    // System Output Level Pad: -6 dB off the
    // outputs, for consumer gear, or none.
    bool padSystemOutput = false;
    // Sound Compiler Warnings: Notify immediately,
    // a dialog to close, or Help window only, the diagnostics alone.
    bool notifyCompilerWarnings = false;
    // The frequently used folders, searched for the files Sounds name and
    // listed by the organizer.
    std::vector<std::string> frequentlyUsedFolders;
    // An external application to edit files of a kind; an empty one is a
    // kind checked whose application is asked for the next time a file of
    // it is edited.
    std::map<TalasFileKind, std::string> fileEditors;
    // Miscellaneous: a help window and an untitled project at startup.
    bool showHelpOnStartup = true;
    bool showUntitledOnStartup = true;
    // The Sound Browser on the right at startup.
    bool showSoundBrowserOnStartup = true;
    // Performance: Update display
    // during disk operations, so that Ctrl+K stops a Sound on the disk and
    // the animated displays go on; off, only Shift+click stops it.
    bool updateDisplayDuringDiskOperations = true;
    // DSP > Configure MIDI: the Default Input Channel, 1 to 16, for the Sounds controlled by
    // MIDI with no channel of their own and the MIDIVoices and MIDIMappers
    // set to channel 0.
    int defaultMidiInputChannel = 1;
    // DSP > Configure MIDI: the channel whose
    // program changes choose the virtual control surface's presets, apart
    // from the Timeline's, or 0 for any channel.
    int vcsProgramChangeChannel = 0;
    // The number each prototype last gave a copy of
    // itself, so that its copies go on numbering for as long as the same
    // preferences are kept.
    std::map<std::string, unsigned> prototypeCopyNumbers;
    // Miscellaneous: Allow stack traces: a script's error tells the messages it happened
    // within too.
    bool allowStackTraces = false;
    // Escape-learning: one key played names !Pitch, or !KeyPitch (the
    // default).
    bool learnOneKeyAsPitch = false;
    // Appearance: a selected
    // Sound highlighted in gray, or blue (the default).
    bool graySelection = false;
    // Speaker Placement: the speakers a
    // Timeline's panned Sounds go between, one an output channel in order;
    // none, the left and right of two channels.
    std::vector<talas::project::SpeakerPosition> speakerPlacement;
    // Spectrum Analyzer: the Info menu's analyzer's length (64, 128 or 256)
    // and window.
    SpectrumAnalyzerSettings spectrumAnalyzer;

    bool operator==(const WorkbenchPreferences&) const = default;
};

// Whether a program change on `channel` (1 to 16) chooses one of the
// surface's presets, given the surface's own channel (0: any).
[[nodiscard]] bool programChangeSelectsVcsPreset(int vcsChannel, int channel) noexcept;

// A DiskRecorder records into the file its FileName
// names: a full path as it is, a name alone into the Program folder of the
// Talas folder; a name without an extension is a WAV file, or an AIFF
// file for a DiskRecorder whose Format is AIFF.
[[nodiscard]] std::filesystem::path diskRecorderFile(
    std::string_view fileName, const std::filesystem::path& programFolder, bool aiff = false);
// The same for any Format: a name without an
// extension takes the one given, the Format's (.wav, .aif, .sf, .sd1 or
// .sd2).
[[nodiscard]] std::filesystem::path diskRecorderFileOfFormat(
    std::string_view fileName, const std::filesystem::path& programFolder, std::string_view extension);

[[nodiscard]] std::string writeWorkbenchPreferences(const WorkbenchPreferences& preferences);
// defaults: what a missing or damaged file gives, the Talas folder among
// the frequently used folders.
[[nodiscard]] WorkbenchPreferences readWorkbenchPreferences(std::string_view text, const WorkbenchPreferences& defaults = {});
// Started with Caps Lock held, the application
// takes the default preferences instead of the file's, in case a damaged
// file keeps it from starting.
[[nodiscard]] WorkbenchPreferences startupWorkbenchPreferences(
    std::string_view text, const WorkbenchPreferences& defaults, bool capsLockHeld);

// The release number and release date at the lower
// left of Preferences, the date as the compiler gives it ("Nov  2 2026")
// written 2026-11-02; the number alone for a date it cannot read.
[[nodiscard]] std::string preferencesReleaseLine(std::string_view version, std::string_view compilerDate);

// The gain the System Output Level Pad leaves the outputs: -6 dB, or 1.
[[nodiscard]] float systemOutputGain(const WorkbenchPreferences& preferences) noexcept;

// The Frequently Used Folders preference: Open shows the folders inside the one selected in the
// upper list, by name, hidden ones left out; Add puts the folder selected
// there at the end of the lower list, once; Remove takes the one selected
// in the lower list out.
[[nodiscard]] std::vector<std::filesystem::path> foldersInside(const std::filesystem::path& folder);
[[nodiscard]] WorkbenchPreferences withFrequentlyUsedFolder(
    WorkbenchPreferences preferences, const std::filesystem::path& folder);
[[nodiscard]] WorkbenchPreferences withoutFrequentlyUsedFolder(WorkbenchPreferences preferences, std::size_t index);

// A file name as the preferences show it: whole, or its name alone.
[[nodiscard]] std::string displayedFileName(std::string_view path, const WorkbenchPreferences& preferences);
// A number with twelve significant digits, or six.
[[nodiscard]] std::string displayedNumber(double value, const WorkbenchPreferences& preferences);

enum class TalasFileOpening : std::uint8_t
{
    ExternalEditor,
    // The kind is checked, its application to be found.
    AskForExternalEditor,
    SampleEditor,
    SpectrumEditor,
    MidiTimeline,
    None,
};

// How Enter or a double-click in the organizer opens a file: in the external
// editor the preferences name for its kind, else Talas's own editor for it;
// File > Open... always takes Talas's own.
[[nodiscard]] TalasFileOpening talasFileOpening(
    TalasFileKind kind, const WorkbenchPreferences& preferences, bool fromOpenCommand = false);

// Ctrl+Space in the File Organizer on a file that is not a
// sample: a Sound file holding the Sound that plays it, an analysis
// resynthesised, a MIDI file on a minimal orchestra, the files it reads
// referenced where they are; none for a file no Sound plays.
struct TalasFilePlayer
{
    talas::project::ProjectDocument soundFile;
    std::string definitionId;
    std::string name;
};

[[nodiscard]] std::optional<TalasFilePlayer> talasFilePlayer(const std::filesystem::path& path);

// A file name dragged from the File Organizer into a parameter field
//: the text a drag carries, the file it names; and
// what the file sets a field to where it makes sense for it, a sample's or
// MIDI file's length in a duration field; none where it does not, the field
// left as it was. A file name field takes the file itself.
[[nodiscard]] std::string talasFileDragDescription(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> talasFileFromDrag(std::string_view description);
[[nodiscard]] std::optional<std::string> talasFileFieldValue(
    const std::filesystem::path& path, talas::expr::ValueDomain domain);

// Open any... and Play...: the kinds of file their
// dialogs list, each with the names it shows; Play... lists those it plays,
// samples and the Sound files whose Sounds it plays.
struct TalasFileType
{
    std::string name;
    std::string wildcard;

    bool operator==(const TalasFileType&) const = default;
};

[[nodiscard]] std::vector<TalasFileType> talasFileTypes(bool playableOnly);

// File > Open...:
// the file types its list offers, a Sound file first, each opening its files
// its own way: a Sound file or Timeline as the project, a Tool in the Tool
// editor (Edit Tool) or running in the Tool window (Use Tool), and Any
// openable file in the editor for its kind, as Open any... does.
enum class TalasOpenAction
{
    Project,
    EditTool,
    UseTool,
    AnyOpenableFile,
};

struct TalasOpenFileType
{
    std::string name;
    std::string wildcard;
    TalasOpenAction action = TalasOpenAction::Project;

    bool operator==(const TalasOpenFileType&) const = default;
};

[[nodiscard]] std::vector<TalasOpenFileType> talasOpenFileTypes();
// A Timeline's file has the extension .talastimeline; here it is a Sound file, the
// project, saved with the Timeline it holds, and opens with the Timeline
// shown.
inline constexpr char timelineFileExtension[] = ".talastimeline";
[[nodiscard]] bool isTimelineFile(const std::filesystem::path& path);

// File > New...: the kinds of file it makes, chosen from a list: a Sound file or a
// Timeline (each a project here, the Timeline shown for the second), a
// Tool, a sample file, a global map, a text file and a Compiled Sound grid
// (a Live Grid of the project).
[[nodiscard]] std::vector<std::string> talasNewFileTypes();

// The text file editor: a .txt file that is no
// RE analysis opens in it, an expressions file among them.
[[nodiscard]] bool talasFileOpensAsText(const std::filesystem::path& path);

// The Tools the Tools menu offers: the Tool files
// in a folder named Tools among the folders given or within one of them, by
// name, each once.
[[nodiscard]] std::vector<std::filesystem::path> toolsInToolsFolders(const std::vector<std::filesystem::path>& folders);
// File > Documentation: the PDF, Markdown and HTML
// files in a folder named Documentation among the folders given or within
// one of them, by name, each once.
[[nodiscard]] std::vector<std::filesystem::path> documentationInDocumentationFolders(
    const std::vector<std::filesystem::path>& folders);
[[nodiscard]] bool talasFileIsOfType(const std::filesystem::path& path, const TalasFileType& type);

// Global map files: a .map file holds a global map's text, opened in the global map
// editor or chosen as the project's map; the Global Maps folder in a
// folder holds Default.map, the Default map's text, written there when it
// is missing.
[[nodiscard]] bool talasFileIsGlobalMap(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::string> globalMapFileText(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::filesystem::path> defaultGlobalMapFile(const std::filesystem::path& folder);

// Ctrl+Up and Ctrl+Down: the window brought to the front
// next, round the list of open windows.
[[nodiscard]] std::size_t cycledWindowIndex(std::size_t count, std::size_t current, int step) noexcept;

// The Sound a file dragged into the Sound list becomes: a
// sample a Sample playing it, a spectrum a SumOfSines, a GA analysis
// GAOscillators, an RE table RE resonators fed its EX file when there is
// one, a MIDI file a Mixer of MIDIVoices, one for each channel.
[[nodiscard]] std::optional<AnalysisExampleSound> soundForTalasFile(
    const talas::project::ProjectDocument& document, const std::filesystem::path& path);
}
