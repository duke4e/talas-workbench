#include "talas/app/SoundBrowserAudition.h"
#include "talas/app/TalasFiles.h"
#include "talas/app/SoundBrowserModel.h"
#include "talas/app/SoundBrowserSurface.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEventValueDiscovery.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
using Kind = talas::app::SoundBrowserKind;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void little(std::vector<std::uint8_t>& bytes, std::uint32_t value, int count)
{
    for (int index = 0; index < count; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

// A mono 16-bit WAV of `frames` frames at 8 kHz, its header giving a pitch
// when one is asked for.
std::vector<std::uint8_t> wav(std::uint32_t frames, std::optional<std::uint32_t> note = std::nullopt)
{
    std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
    const std::uint32_t sampler = note.has_value() ? 8 + 36 : 0;
    little(bytes, 36 + sampler + frames * 2, 4);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    little(bytes, 16, 4);
    little(bytes, 1, 2);
    little(bytes, 1, 2);
    little(bytes, 8000, 4);
    little(bytes, 16000, 4);
    little(bytes, 2, 2);
    little(bytes, 16, 2);
    if (note.has_value())
    {
        bytes.insert(bytes.end(), {'s', 'm', 'p', 'l'});
        little(bytes, 36, 4);
        little(bytes, 0, 4);
        little(bytes, 0, 4);
        little(bytes, 125000, 4);
        little(bytes, *note, 4);
        for (int field = 0; field < 5; ++field)
            little(bytes, 0, 4);
    }
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    little(bytes, frames * 2, 4);
    bytes.resize(bytes.size() + frames * 2, 0);
    return bytes;
}

void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << text;
}

talas::project::ProjectDocument soundFile()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition reverb;
    reverb.definitionId = "reverb";
    reverb.displayName = "Effects-EUverb";
    reverb.description = "A reverb for any input";
    reverb.inputs.push_back({"in", talas::core::PortType::Audio, "gain", "audioIn"});
    reverb.outputs.push_back({"audioOut", talas::core::PortType::Audio, "gain", "leftOut"});
    reverb.graph.nodes.push_back({"gain", "talas.gain", {}});
    document.soundDefinitions.push_back(reverb);
    talas::project::ProjectSoundDefinition keyboard;
    keyboard.definitionId = "keyboard";
    keyboard.displayName = "Alien KBD";
    keyboard.outputs.push_back({"audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    keyboard.graph.nodes.push_back({"osc", "talas.oscillator", {{"frequency", "!KeyPitch", {}, {}, {}, {}}}});
    document.soundDefinitions.push_back(keyboard);
    return document;
}

struct Library
{
    std::filesystem::path root;
    Library()
    {
        root = std::filesystem::temp_directory_path() / ("talas-sound-browser-" + std::to_string(std::rand()));
        std::filesystem::remove_all(root);
        write(root / "Factory Sound Library" / "An Overview.talas.json", talas::project::serializeProjectDocument(soundFile()));
        write(root / "Samples" / "animals" / "indoor jungle.wav", wav(8000, 62));
        write(root / "Samples" / "bucket2.wav", wav(400000));
        write(root / "Samples" / "notes.txt", std::string{"not a Sound's"});
    }
    ~Library()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

[[nodiscard]] std::vector<std::string> names(const std::vector<talas::app::SoundBrowserRow>& rows)
{
    std::vector<std::string> result;
    for (const auto& row : rows)
        result.push_back(std::string(row.depth * 2, ' ') + row.name);
    return result;
}

// Folders indented by depth, opened and closed by
// their triangles, a Sound file holding its Sounds.
void testTheBrowserListsFoldersFilesAndSounds()
{
    Library library;
    talas::app::SoundBrowserModel browser;
    browser.setRoots({library.root});
    require(browser.rows().size() == 1, "a closed root shows itself alone");
    browser.toggle(library.root);
    browser.toggle(library.root / "Factory Sound Library");
    browser.toggle(library.root / "Factory Sound Library" / "An Overview.talas.json");
    browser.toggle(library.root / "Samples");
    const auto rows = browser.rows();
    const std::vector<std::string> expected{library.root.filename().string(), "  Factory Sound Library", "    An Overview",
        "      Alien KBD", "      Effects-EUverb", "  Samples", "    animals", "    bucket2.wav"};
    require(names(rows) == expected, "the folders, the Sound file and its Sounds, indented by depth");
    require(rows[2].kind == Kind::SoundFile && rows[3].kind == Kind::Sound && rows[3].definitionId == "keyboard"
            && rows[7].kind == Kind::Sample,
        "each row knows its kind");
    require(rows[4].processesInput && !rows[3].processesInput, "a Sound with an input to replace shows a cyan arrow");
    browser.toggle(library.root / "Factory Sound Library" / "An Overview.talas.json");
    require(browser.rows().size() == expected.size() - 2, "closing the Sound file hides its Sounds");
    require(talas::app::soundBrowserKindColour(Kind::SoundFile) != talas::app::soundBrowserKindColour(Kind::Sample),
        "the colour of a name tells its kind");
    // Sound files' names and, indented below them, their Sounds'
    // are blue.
    const auto blue = [](std::uint32_t colour) {
        return (colour & 0xffu) > ((colour >> 8) & 0xffu) && (colour & 0xffu) > ((colour >> 16) & 0xffu);
    };
    require(blue(talas::app::soundBrowserKindColour(Kind::SoundFile)) && blue(talas::app::soundBrowserKindColour(Kind::Sound))
            && rows[3].depth > rows[2].depth,
        "a Sound file's name and its Sounds' names, indented, are blue");

    // A Sound file saved meanwhile, and a new folder of one, show
    // after the Refresh button.
    browser.toggle(library.root / "Factory Sound Library" / "An Overview.talas.json");
    auto changed = soundFile();
    changed.soundDefinitions.back().displayName = "Bucket Vocoder";
    write(library.root / "Factory Sound Library" / "An Overview.talas.json", talas::project::serializeProjectDocument(changed));
    write(library.root / "My Sound Library" / "Tutorial Sounds.talas.json", talas::project::serializeProjectDocument(changed));
    const auto shown = [&](std::string_view name) {
        const auto now = browser.rows();
        return std::ranges::find(now, name, &talas::app::SoundBrowserRow::name) != now.end();
    };
    require(!shown("Bucket Vocoder"), "the Sound file's Sounds as they were read");
    browser.refresh();
    browser.toggle(library.root / "My Sound Library");
    require(shown("Bucket Vocoder") && !shown("Alien KBD") && shown("Tutorial Sounds"),
        "Refresh reads the Sound files again; the new folder and Sound file can be found");
}

// A browser spawned on a folder by a double-click
// lists that folder's files alone, under the folder's name.
void testASpawnedBrowserOfOneFolder()
{
    Library library;
    talas::app::SoundBrowserModel spawned;
    spawned.setRoots({library.root / "Samples"});
    spawned.toggle(library.root / "Samples");
    require(names(spawned.rows()) == std::vector<std::string>{"Samples", "  animals", "  bucket2.wav"},
        "the spawned browser should list just the files of the folder");
    require(talas::app::soundBrowserTitle(library.root / "Samples") == "Samples" && talas::app::soundBrowserTitle(std::nullopt) == "Sound Browser",
        "the spawned browser should have the folder's name as its label");
    require(!spawned.find("alien", std::nullopt).has_value() && spawned.find("jungle", std::nullopt).has_value(),
        "a spawned browser should search its own folder only");
}

// A folder's name in the text colour,
// a Sound file's in blue underlined, a Sound's in blue plain; closed, a
// folder or Sound file is faded beside a sideways triangle, open saturated
// beside a downward one; a Sound that processes its input has an arrow.
void testHowTheRowsLook()
{
    using talas::app::SoundBrowserTriangle;
    talas::app::SoundBrowserRow folder{"/tmp", {}, "tmp", 0, Kind::Folder, false, false};
    talas::app::SoundBrowserRow file{"/tmp/a.talas.json", {}, "a", 1, Kind::SoundFile, false, false};
    talas::app::SoundBrowserRow sound{"/tmp/a.talas.json", "s", "s", 2, Kind::Sound, false, true};
    const auto closedFolder = talas::app::soundBrowserRowLook(folder);
    require(!closedFolder.colour.has_value() && closedFolder.faded && closedFolder.triangle == SoundBrowserTriangle::Sideways
            && !closedFolder.underlined,
        "a closed folder's name in the text colour, faded beside a sideways triangle");
    folder.open = true;
    const auto openFolder = talas::app::soundBrowserRowLook(folder);
    require(!openFolder.faded && openFolder.triangle == SoundBrowserTriangle::Down,
        "an open folder saturated beside a downward triangle");
    const auto closedFile = talas::app::soundBrowserRowLook(file);
    file.open = true;
    const auto openFile = talas::app::soundBrowserRowLook(file);
    require(closedFile.colour == std::optional<std::uint32_t>{talas::app::soundBrowserKindColour(Kind::SoundFile)}
            && closedFile.underlined && closedFile.faded && closedFile.triangle == SoundBrowserTriangle::Sideways
            && !openFile.faded && openFile.triangle == SoundBrowserTriangle::Down,
        "a Sound file blue and underlined, a lighter blue while closed, darker again open");
    const auto soundLook = talas::app::soundBrowserRowLook(sound);
    require(soundLook.colour == std::optional<std::uint32_t>{talas::app::soundBrowserKindColour(Kind::Sound)} && !soundLook.underlined
            && !soundLook.faded && soundLook.triangle == SoundBrowserTriangle::None && soundLook.arrow,
        "a Sound blue without underlining, an arrow beside one that modifies its input");
    sound.processesInput = false;
    require(!talas::app::soundBrowserRowLook(sound).arrow, "a Sound that makes its own signal has no arrow");
}

// The Expressions Library is a folder
// of text files beginning with EXPR; a file opens by its triangle onto its
// expressions, and one selected shows its comment and then its code at the
// bottom; dragged into a field it carries its code. Other text files are
// not listed.
void testExpressionsFilesListTheirExpressions()
{
    Library library;
    const auto folder = library.root / "Expressions Library";
    (void) talas::app::writeExpressionsLibrary(folder);
    write(folder / "notes.txt", std::string{"not expressions"});
    talas::app::SoundBrowserModel browser;
    browser.setRoots({folder});
    browser.toggle(folder);
    auto rows = browser.rows();
    const auto row = [&rows](std::string_view name) {
        const auto found = std::ranges::find(rows, name, &talas::app::SoundBrowserRow::name);
        require(found != rows.end(), "the browser should list " + std::string{name});
        return *found;
    };
    require(rows.front().name == "Expressions Library" && rows.size() == talas::app::expressionsLibraryFiles().size() + 1
            && std::ranges::none_of(rows, [](const auto& listed) { return listed.name == "notes.txt"; }),
        "the Expressions Library folder lists its expressions files, and no other text file");
    for (const auto* file : {"keyboard expressions.txt", "fader expressions.txt", "samples & looping.txt", "BPM.txt",
             "control functions.txt", "fader warping.txt", "arrays.txt", "sequences.txt"})
    {
        const auto listed = row(file);
        require(listed.kind == Kind::ExpressionsFile && !listed.open
                && talas::app::soundBrowserRowLook(listed).triangle == talas::app::SoundBrowserTriangle::Sideways,
            std::string{""} + file + " should be listed closed beside its triangle");
        browser.toggle(listed.path);
    }
    require(talas::app::soundBrowserActivation(Kind::ExpressionsFile) == talas::app::SoundBrowserActivation::Toggle,
        "an expressions file opens and closes");
    rows = browser.rows();
    // Opened, each shows its expressions; one selected shows its comment,
    // then its code, and dragged it carries its code.
    for (const auto* name : {"Pitch plus an interval", "Fader inverted (max=10)",
             "Backwards & forwards through a GenericSource or Sample", "Loop a sample at any !BPM",
             "(-1 -> 1) repeating linear in sample's duration", "(0 -> 1-> 0) repeating triangle in sample's duration",
             "Nonlinear fader", "!Amp fader array algorithmically", "Frequency (pitch) array spaced !Interval apart",
             "Musical scale steps using a controller", "Arpeggiator array", "Arpeggiator array (variable intervals)",
             "Arpeggiator random access array (variable)"})
    {
        const auto listed = row(name);
        const auto entry = talas::app::soundBrowserExpression(listed);
        const auto information = talas::app::soundBrowserInformation(listed);
        require(listed.kind == Kind::Expression && entry.has_value() && information.size() == 3
                && information[1] == entry->comment && information[2] == entry->expression,
            std::string{""} + name + " should show its comment and below it its code");
        require(talas::app::expressionFromLibraryDrag(talas::app::soundBrowserDragDescription(listed))
                == std::optional<std::string>{entry->expression},
            std::string{""} + name + " dragged into a field should carry its code");
    }
    require(talas::app::soundBrowserActivation(Kind::Expression) == talas::app::SoundBrowserActivation::Nothing,
        "an expression is dragged, not opened");
    const auto found = browser.find("Nonlinear", std::nullopt);
    require(found.has_value() && found->name == "Nonlinear fader", "Find finds an expression by its name");
    browser.showOnly(Kind::Sample);
    require(names(browser.rows()) == std::vector<std::string>{"Expressions Library"},
        "the expressions hide with their box unchecked");
}

// A click on a colour shows that kind only, Shift+Click
// adds or removes one, the grey box shows them all.
void testColouredBoxesShowAndHideKinds()
{
    Library library;
    talas::app::SoundBrowserModel browser;
    browser.setRoots({library.root});
    for (const auto& folder : {library.root, library.root / "Factory Sound Library", library.root / "Samples"})
        browser.toggle(folder);
    browser.toggle(library.root / "Factory Sound Library" / "An Overview.talas.json");
    const auto count = [&browser](Kind kind) {
        const auto rows = browser.rows();
        return std::ranges::count(rows, kind, &talas::app::SoundBrowserRow::kind);
    };
    browser.showOnly(Kind::Sample);
    require(count(Kind::Sample) == 1 && count(Kind::Sound) == 0 && count(Kind::Folder) == 4,
        "clicking a colour shows files of that kind only, the folders still");
    browser.toggleKind(Kind::Sound);
    require(count(Kind::Sound) == 2 && count(Kind::SoundFile) == 1, "Shift+Click adds a kind");
    browser.toggleKind(Kind::Sound);
    require(count(Kind::Sound) == 0, "Shift+Click on a shown kind hides it");
    browser.showAll();
    require(count(Kind::Sound) == 2 && count(Kind::Sample) == 1, "the grey box shows every kind");
    // Each box's fly-by help names its kind of file.
    require(talas::app::soundBrowserKindName(Kind::Sound) == "Sound" && talas::app::soundBrowserKindName(Kind::Sample) == "Sample"
            && talas::app::soundBrowserKindName(Kind::Midi) == "MIDI file",
        "the fly-by help should name each colour's kind of file");
}

// The information on the selected Sound or file.
void testInformationOnTheSelectedRow()
{
    Library library;
    talas::app::SoundBrowserModel browser;
    browser.setRoots({library.root});
    for (const auto& folder : {library.root, library.root / "Factory Sound Library", library.root / "Samples",
             library.root / "Samples" / "animals"})
        browser.toggle(folder);
    browser.toggle(library.root / "Factory Sound Library" / "An Overview.talas.json");
    const auto rows = browser.rows();
    const auto row = [&rows](std::string_view name) {
        return *std::ranges::find(rows, name, &talas::app::SoundBrowserRow::name);
    };
    const auto reverb = talas::app::soundBrowserInformation(row("Effects-EUverb"));
    require(reverb.size() >= 2 && reverb[0] == "Sound · Effects-EUverb" && reverb[1] == "A reverb for any input",
        "a Sound's information gives its kind, name and comment");
    const auto keyboard = talas::app::soundBrowserInformation(row("Alien KBD"));
    require(std::ranges::find(keyboard, std::string{"MIDI"}) != keyboard.end()
            && std::ranges::find(reverb, std::string{"MIDI"}) == reverb.end(),
        "MIDI shows for a Sound that expects MIDI keys");
    const auto jungle = talas::app::soundBrowserInformation(row("indoor jungle.wav"));
    require(jungle.size() == 6 && jungle[0] == "Sample · indoor jungle.wav"
            && jungle[1] == "Directory " + (library.root / "Samples" / "animals").string()
            && jungle[2].starts_with("Modified ") && jungle[3] == "Duration 1 s" && jungle[4] == "Pitch 62 nn"
            && jungle[5] == "8000 samples · 8000 Hz · 16 bits · mono",
        "a sample's type and name, folder, last change, duration, pitch from its header, and its samples, "
        "rate, bit depth and channels");
    // A Sound's duration, the longest sample it plays.
    auto players = soundFile();
    players.assets.push_back({"jungle", talas::project::ProjectAssetKind::Audio, "indoor jungle.wav"});
    players.assets.back().sampleRate = 8000.0;
    players.assets.back().frameCount = 20000;
    talas::project::ProjectSoundDefinition player;
    player.definitionId = "player";
    player.displayName = "Jungle player";
    player.graph.nodes.push_back({"sample", "talas.sample", {{"sample", {}, {}, {}, "jungle", {}}}});
    players.soundDefinitions.push_back(player);
    write(library.root / "Players.talas.json", talas::project::serializeProjectDocument(players));
    talas::app::SoundBrowserRow playerRow{library.root / "Players.talas.json", "player", "Jungle player", 1,
        talas::app::SoundBrowserKind::Sound};
    const auto played = talas::app::soundBrowserInformation(playerRow);
    require(played.size() >= 2 && played[0] == "Sound · Jungle player" && played[1] == "Duration 2.5 s",
        "a Sound's information gives its duration, the longest sample it plays");
    require(std::ranges::none_of(reverb, [](const std::string& line) { return line.starts_with("Duration"); }),
        "a Sound playing no sample has no duration to give");
    require(talas::app::samplePitchNoteNumber(library.root / "Samples" / "bucket2.wav") == 60,
        "a sample with no pitch in its header is middle C, 60 nn");
    require(talas::app::soundBrowserLoops(1.0) && !talas::app::soundBrowserLoops(50.0),
        "samples under 30 seconds loop when played in the browser");
    // Every kind of file the browser lists plays in it.
    using talas::app::SoundBrowserKind;
    using talas::app::SoundBrowserPlayer;
    require(talas::app::soundBrowserPlayer(SoundBrowserKind::Sample) == SoundBrowserPlayer::Sample
            && talas::app::soundBrowserPlayer(SoundBrowserKind::Sound) == SoundBrowserPlayer::Sound
            && talas::app::soundBrowserPlayer(SoundBrowserKind::Spectrum) == SoundBrowserPlayer::FileSound
            && talas::app::soundBrowserPlayer(SoundBrowserKind::Re) == SoundBrowserPlayer::FileSound
            && talas::app::soundBrowserPlayer(SoundBrowserKind::Midi) == SoundBrowserPlayer::FileSound
            && talas::app::soundBrowserPlayer(SoundBrowserKind::SoundFile) == SoundBrowserPlayer::Timeline
            && talas::app::soundBrowserPlayer(SoundBrowserKind::Folder) == SoundBrowserPlayer::None,
        "Sounds, analysis files, Timelines, MIDI files and samples should each play in the browser");
    require(!talas::app::soundBrowserLoops(1.0, true),
        "Play clicked with Control held defeats looping, playing the sample once");
}

// Find looks after the selection for a name holding
// the text, opening its folders; Find Again goes on from there.
void testFindingAName()
{
    Library library;
    talas::app::SoundBrowserModel browser;
    browser.setRoots({library.root});
    const auto first = browser.find("ALIEN", std::nullopt);
    require(first.has_value() && first->name == "Alien KBD" && first->kind == Kind::Sound,
        "Find finds a Sound by part of its name, ignoring case");
    require(browser.isOpen(library.root / "Factory Sound Library" / "An Overview.talas.json") && browser.isOpen(library.root),
        "the folders and Sound file holding the match open");
    const auto jungle = browser.find("jungle", first);
    require(jungle.has_value() && jungle->kind == Kind::Sample, "Find finds samples as well as Sounds");
    const auto again = browser.find("alien", jungle);
    require(again.has_value() && again->name == "Alien KBD", "Find again past the last match should wrap round to the top");
    require(!browser.find("alien", again).has_value(), "with no other match Find again should find nothing more");
    // A click or a key stops a search, which then finds and
    // opens nothing.
    talas::app::SoundBrowserModel stopping;
    stopping.setRoots({library.root});
    int asked = 0;
    require(!stopping.find("alien", std::nullopt, [&asked] { return ++asked > 2; }).has_value() && asked > 2
            && !stopping.isOpen(library.root),
        "a stopped search should find and open nothing");
    require(stopping.find("alien", std::nullopt, [] { return false; }).has_value(), "a search not stopped should find as before");

    // The open folders first, then all of them; a kind whose box is
    // not checked is not searched.
    write(library.root / "Factory Sound Library" / "bucket brigade.wav", wav(800));
    talas::app::SoundBrowserModel opened;
    opened.setRoots({library.root});
    opened.toggle(library.root);
    opened.toggle(library.root / "Samples");
    const auto open = opened.find("bucket", std::nullopt);
    require(open.has_value() && open->name == "bucket2.wav", "Find should search the open folders first");
    const auto closed = opened.find("bucket", open);
    require(closed.has_value() && closed->name == "bucket brigade.wav",
        "with no other match in the open folders Find should search every folder");
    opened.showOnly(Kind::Sound);
    require(!opened.find("bucket", std::nullopt).has_value() && opened.find("alien", std::nullopt).has_value(),
        "Find should pass over the kinds not shown");

    // Searching on finds analysis files and Timelines (each a Sound
    // file of its own here) as well as Sounds and samples.
    write(library.root / "Samples" / "choir s256.spc", std::string{"spectrum"});
    talas::app::SoundBrowserModel more;
    more.setRoots({library.root});
    const auto analysis = more.find("choir", std::nullopt);
    const auto timeline = more.find("overview", std::nullopt);
    require(analysis.has_value() && analysis->kind == Kind::Spectrum && timeline.has_value()
            && timeline->kind == Kind::SoundFile,
        "Find should find analysis files and Timelines as well as Sounds");
}

// The replacement input, an audio input stepped
// round, a sample or a Sound designated, the connection button.
void testTheReplacementInput()
{
    talas::app::SoundBrowserReplacementInput input;
    require(talas::app::soundBrowserInputText(input) == "Input 1", "the field shows the audio input, Input 1");
    input = talas::app::nextAudioInput(input, 4);
    require(input.audioInput == 2 && input.connected, "the audio input button goes to the next input");
    for (int step = 0; step < 3; ++step)
        input = talas::app::nextAudioInput(input, 4);
    require(input.audioInput == 1, "past the highest numbered input it wraps round to Input 1");
    talas::app::SoundBrowserRow sample{"/tmp/bucket2.wav", {}, "bucket2.wav", 1, Kind::Sample, false, false};
    const auto substitute = talas::app::substituteInput(sample);
    require(substitute.has_value() && substitute->kind == talas::app::SoundBrowserInputKind::Sample
            && talas::app::soundBrowserInputText(*substitute) == "bucket2.wav" && substitute->connected,
        "the substitute button makes the selected sample the input, its name in the field, connected");
    talas::app::SoundBrowserRow folder{"/tmp", {}, "tmp", 0, Kind::Folder, false, false};
    require(!talas::app::substituteInput(folder).has_value(), "a folder cannot be an input");
}

[[nodiscard]] const talas::project::ProjectNode& feeding(const talas::project::ProjectDocument& document)
{
    const auto connection = std::ranges::find(document.graph.connections, std::string{"auditionSound"}, &talas::core::Connection::toNode);
    require(connection != document.graph.connections.end(), "the Sound should be fed its input");
    return *std::ranges::find(document.graph.nodes, connection->fromNode, &talas::project::ProjectNode::instanceId);
}

// The browser plays a Sound with the replacement
// input in place of its input, an audio input, a sample or another Sound,
// and with the default sine when disconnected; dragged into the project it
// keeps the combination as a new Sound.
void testPlayingASoundThroughTheReplacementInput()
{
    Library library;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto file = soundFile();
    talas::app::SoundBrowserReplacementInput input;
    const auto plain = talas::app::soundBrowserAuditionDocument(file, "reverb", input, std::nullopt, registry);
    require(plain.ok() && feeding(*plain.document).typeId == "talas.oscillator", "disconnected, the Sound hears its default input: " + plain.diagnostic);
    input = talas::app::nextAudioInput(input, 4);
    const auto live = talas::app::soundBrowserAuditionDocument(file, "reverb", input, std::nullopt, registry);
    require(live.ok() && feeding(*live.document).typeId == "talas.audioInput", "Input 2 feeds the Sound live: " + live.diagnostic);
    const auto& channels = feeding(*live.document).parameters;
    const auto second = std::ranges::find(channels, std::string{"channel2"}, &talas::project::ProjectParameterExpression::parameterId);
    require(second != channels.end() && second->boolean == std::optional<bool>{true}, "the second audio input channel");
    const auto sample = talas::app::substituteInput({library.root / "Samples" / "bucket2.wav", {}, "bucket2.wav", 1, Kind::Sample, false, false});
    const auto sampled = talas::app::soundBrowserAuditionDocument(file, "reverb", *sample, std::nullopt, registry);
    require(sampled.ok() && feeding(*sampled.document).typeId == "talas.diskPlayer"
            && std::ranges::any_of(sampled.document->assets, [](const auto& asset) { return asset.id == "soundBrowserInput"; }),
        "a designated sample plays into the Sound: " + sampled.diagnostic);
    const auto sound = talas::app::substituteInput({library.root / "Factory Sound Library" / "An Overview.talas.json", "keyboard", "Alien KBD", 2,
        Kind::Sound, false, false});
    const auto sounded = talas::app::soundBrowserAuditionDocument(file, "reverb", *sound, file, registry);
    require(sounded.ok() && feeding(*sounded.document).soundDefinitionId.has_value(), "any Sound can be the input: " + sounded.diagnostic);
    auto disconnected = *sample;
    disconnected.connected = false;
    require(feeding(*talas::app::soundBrowserAuditionDocument(file, "reverb", disconnected, std::nullopt, registry).document).typeId == "talas.oscillator",
        "the connection button disengages the replacement input");

    const talas::project::ProjectDocument project;
    const auto kept = talas::app::soundBrowserImport(project, file, "reverb", *sample, std::nullopt, registry);
    require(kept.ok(), "the combination should be added: " + kept.diagnostic);
    const auto combination = std::ranges::find(kept.document->soundDefinitions, kept.rootDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    require(combination != kept.document->soundDefinitions.end() && combination->displayName == "Effects-EUverb (bucket2.wav)"
            && combination->inputs.empty() && !combination->outputs.empty(),
        "dragged in with the input connected, the Sound and its input become one new Sound");
    const auto original = talas::app::soundBrowserImport(project, file, "reverb", disconnected, std::nullopt, registry);
    require(original.ok() && std::ranges::find(original.document->soundDefinitions, original.rootDefinitionId,
                                 &talas::project::ProjectSoundDefinition::definitionId)->displayName == "Effects-EUverb",
        "with the replacement input disconnected the Sound comes in with its own input");
}

// A Sound file's Sounds are listed alphabetically,
// letters' case aside, whatever their order in the file.
void testTheSoundsAreListedAlphabetically()
{
    talas::project::ProjectDocument file;
    for (const auto& [id, name] : {std::pair{"c", "Stressed out cartoon"}, std::pair{"a", "bell"}, std::pair{"b", "Arpeggio"}})
    {
        talas::project::ProjectSoundDefinition sound;
        sound.definitionId = id;
        sound.displayName = name;
        file.soundDefinitions.push_back(sound);
    }
    std::vector<std::string> names;
    for (const auto& sound : talas::app::soundBrowserSounds(file))
        names.push_back(sound.name);
    require(names == std::vector<std::string>{"Arpeggio", "bell", "Stressed out cartoon"},
        "the Sounds should be listed alphabetically");
}

// A Sound with a replaceable input, one of its own
// Sounds, has the cyan arrow; the browser plays it with its own Sound there
// until the global input is connected, which then takes that Sound's place.
void testTheGlobalInputReplacesTheReplaceableInput()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument file;
    talas::project::ProjectSoundDefinition doppler;
    doppler.definitionId = "doppler";
    doppler.displayName = "Effects-Doppler shift";
    doppler.graph.nodes = {{"Mixer206", "talas.oscillator", {}}, {"doppler shifter", "talas.vca", {}}};
    doppler.graph.connections = {{"Mixer206", "audioOut", "doppler shifter", "audioIn"}};
    doppler.outputs.push_back({"audioOut", talas::core::PortType::Audio, "doppler shifter", "audioOut"});
    doppler.inputs.push_back({"replaceableInput", talas::core::PortType::Audio, "Mixer206", "audioOut", true});
    file.soundDefinitions.push_back(doppler);
    require(talas::app::soundBrowserSounds(file).front().processesInput, "the cyan arrow beside a Sound with a replaceable input");
    talas::app::SoundBrowserReplacementInput input;
    input.connected = false;
    const auto alone = talas::app::soundBrowserAuditionDocument(file, "doppler", input, std::nullopt, registry);
    require(alone.ok() && std::ranges::none_of(alone.document->graph.connections, [](const talas::core::Connection& line) {
        return line.toNode == "auditionSound";
    }), "disconnected, the Sound plays with its own Sound as its input: " + alone.diagnostic);
    input = talas::app::nextAudioInput(input, 4);
    const auto live = talas::app::soundBrowserAuditionDocument(file, "doppler", input, std::nullopt, registry);
    require(live.ok() && feeding(*live.document).typeId == "talas.audioInput"
            && std::ranges::any_of(live.document->graph.connections, [](const talas::core::Connection& line) {
                   return line.toNode == "auditionSound" && line.toPort == "replaceableInput";
               }),
        "the global input is substituted for the replaceable input: " + live.diagnostic);
}

// A Sound played in the Sound Browser shows its
// surface, the widgets its Sound file keeps. Played with the substitute
// input connection button down, the surface takes no changes, nor does it
// once the button is disengaged until the Sound is played again; then it
// lets you change it, and its new arrangement is saved with the Sound in
// its Sound file.
void testTheBrowsersSurfaceTakesChangesOncePlayedWithoutTheSubstituteInput()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument file;
    talas::project::ProjectSoundDefinition granular;
    granular.definitionId = "granular";
    granular.displayName = "Granular Synth Spectrum Display";
    granular.inputs.push_back({"in", talas::core::PortType::Audio, "level", "audioIn"});
    granular.outputs.push_back({"audioOut", talas::core::PortType::Audio, "level", "leftOut"});
    granular.graph.nodes.push_back({"level", "talas.gain", {{"left", "!Vol", {}, {}, {}, {}}, {"right", "!Amp", {}, {}, {}, {}}}});
    file.soundDefinitions.push_back(granular);
    // The file keeps a widget for Vol: a fader from 0 to 2 at a place of its
    // own, saved at 0.8.
    auto volume = talas::project::makeDefaultVirtualControl("Vol", talas::expr::ValueDomain::Scalar);
    volume.minimum = 0.0;
    volume.maximum = 2.0;
    volume.value = 0.8;
    volume.widget.label = "Volume";
    volume.widget.bounds = talas::project::VirtualControlBounds{0, 0, 80, 200};
    file.virtualControls.push_back(volume);
    talas::project::reconcileProjectPresentation(file);

    const auto widget = [](const talas::app::SoundBrowserSurface& surface, const std::string& name) {
        const auto found = std::ranges::find(surface.widgets, name, &talas::project::VirtualControl::eventName);
        require(found != surface.widgets.end(), "the surface should show a widget for " + name);
        return *found;
    };
    const auto played = [&](const talas::app::SoundBrowserReplacementInput& input) {
        const auto audition = talas::app::soundBrowserAuditionDocument(file, "granular", input, std::nullopt, registry);
        require(audition.ok(), "the browser should play the Sound: " + audition.diagnostic);
        const auto surface = talas::app::soundBrowserSurface(file, "granular", input, *audition.document, registry);
        require(surface.ok(), "a Sound played in the browser shows its surface: " + surface.diagnostic);
        // Its values are those the browser plays the Sound at.
        const auto playing = talas::project::discoverProjectEventValueControls(*audition.document, registry);
        for (const auto& control : playing.missingControls)
            require(widget(*surface.surface, control.eventName).value == control.value,
                "the surface should show the value the browser plays " + control.eventName + " at");
        return *surface.surface;
    };

    // The substitute input connection button down.
    auto input = talas::app::nextAudioInput({}, 4);
    require(input.connected, "the substitute input connection button should be down");
    const auto through = played(input);
    require(through.name == "Granular Synth Spectrum Display" && widget(through, "Vol").widget.label == "Volume"
            && widget(through, "Vol").maximum == 2.0 && widget(through, "Amp").maximum == 1.0,
        "the surface shows the widgets its Sound file keeps, a new one for an Event Value it keeps none for");
    require(through.playedThroughSubstituteInput && !talas::app::soundBrowserSurfaceTakesChanges(through)
            && talas::app::soundBrowserSurfaceNote(through).find("substitute input") != std::string::npos,
        "played while the substitute input button was down, the surface says it takes no changes");
    require(!talas::app::soundBrowserSurfaceValueSet(through, "Vol", 1.0).has_value()
            && !talas::app::soundBrowserSurfaceDiceRolled(through, 7).has_value()
            && !talas::app::soundBrowserSurfaceFrozenToggled(through, "Vol").has_value()
            && !talas::app::soundBrowserSurfacePlaced(through, {{"Vol", {10, 20, 60, 180}}}).has_value()
            && !talas::app::soundBrowserSurfaceWidgetSet(through, "Amp", talas::project::VirtualControlKind::Toggle, 0.0, 1.0).has_value()
            && !talas::app::soundFileWithBrowserSurface(file, through).has_value(),
        "nothing happens: the surface takes no values, dice, freezing, rearranging or saving");

    // The button disengaged, but the Sound not played again.
    input.connected = false;
    require(!talas::app::soundBrowserSurfaceTakesChanges(through) && !talas::app::soundBrowserSurfaceValueSet(through, "Vol", 1.0).has_value(),
        "disengaging the button alone does not let the surface change; the Sound is to be played again");

    // Played again: this time it lets you make changes to it.
    auto surface = played(input);
    require(talas::app::soundBrowserSurfaceTakesChanges(surface), "played again with the button disengaged, the surface takes changes");
    auto changed = talas::app::soundBrowserSurfaceValueSet(surface, "Vol", 5.0);
    require(changed.has_value() && widget(*changed, "Vol").value == 2.0, "a value typed in takes effect, within the widget's range");
    changed = talas::app::soundBrowserSurfaceFrozenToggled(*changed, "Vol");
    require(changed.has_value() && changed->frozen.contains("Vol"), "clicking a name freezes it");
    changed = talas::app::soundBrowserSurfaceDiceRolled(*changed, 7);
    require(changed.has_value() && widget(*changed, "Vol").value == 2.0 && widget(*changed, "Amp").value >= 0.0
            && widget(*changed, "Amp").value <= 1.0,
        "the dice set the active parameters within their ranges and leave a frozen one where it was");
    changed = talas::app::soundBrowserSurfacePlaced(*changed, {{"Vol", {10, 20, 60, 180}}, {"Amp", {80, 20, 60, 180}}});
    require(changed.has_value() && changed->rearranged && changed->arrangement == talas::project::VirtualControlLayout::Fixed
            && widget(*changed, "Vol").widget.bounds == std::optional<talas::project::VirtualControlBounds>{{10, 20, 60, 180}},
        "the widgets can be rearranged");
    changed = talas::app::soundBrowserSurfaceWidgetSet(*changed, "Amp", talas::project::VirtualControlKind::Toggle, 0.0, 1.0);
    require(changed.has_value() && widget(*changed, "Amp").kind == talas::project::VirtualControlKind::Toggle,
        "a widget's type can be changed");
    require(!talas::app::soundBrowserSurfaceWidgetSet(*changed, "Amp", talas::project::VirtualControlKind::Fader, 1.0, 1.0).has_value()
            && !talas::app::soundBrowserSurfacePlaced(*changed, {{"Nothing", {0, 0, 10, 10}}}).has_value(),
        "an empty range or a widget the surface does not have is refused");

    // Saved with the Sound on the disk.
    const auto saved = talas::app::soundFileWithBrowserSurface(file, *changed);
    require(saved.has_value() && saved->virtualControlLayout == talas::project::VirtualControlLayout::Fixed,
        "the new arrangement is saved with the Sound");
    const auto read = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(*saved));
    require(read.ok(), "the Sound file with its new surface should read back: " + (read.errors.empty() ? std::string{} : read.errors.front().message));
    const auto keptVolume = std::ranges::find(read.document.virtualControls, std::string{"Vol"}, &talas::project::VirtualControl::eventName);
    const auto keptAmp = std::ranges::find(read.document.virtualControls, std::string{"Amp"}, &talas::project::VirtualControl::eventName);
    require(keptVolume != read.document.virtualControls.end() && keptAmp != read.document.virtualControls.end()
            && keptVolume->widget.bounds == std::optional<talas::project::VirtualControlBounds>{{10, 20, 60, 180}}
            && keptVolume->widget.label == "Volume" && keptVolume->value == 0.8
            && keptAmp->kind == talas::project::VirtualControlKind::Toggle,
        "the Sound file keeps the widgets' places and types, a kept widget its saved value");
    file = read.document;
    const auto again = played(input);
    require(widget(again, "Vol").widget.bounds == std::optional<talas::project::VirtualControlBounds>{{10, 20, 60, 180}}
            && widget(again, "Amp").kind == talas::project::VirtualControlKind::Toggle,
        "played again, the Sound shows the arrangement saved with it");
}
}

// A Sound dragged out of the browser
// carries its Sound file and its definition, a sample or analysis the file
// itself, as the File Organizer drags it; folders and Sound files stay.
void testDraggingRowsOutOfTheBrowser()
{
    const talas::app::SoundBrowserRow sound{"/library/An Overview.talas.json", "vocoder", "Freq/Time Scaling Vocoder", 1, Kind::Sound};
    const auto carried = talas::app::soundBrowserDrag(talas::app::soundBrowserDragDescription(sound));
    require(carried == talas::app::SoundBrowserDrag{"/library/An Overview.talas.json", "vocoder"},
        "a Sound dragged out should carry its Sound file and definition");
    const talas::app::SoundBrowserRow sample{"/library/SwS - 127 Gearbox.aif", {}, "SwS - 127 Gearbox.aif", 1, Kind::Sample};
    const auto description = talas::app::soundBrowserDragDescription(sample);
    require(description == talas::app::talasFileDragDescription(sample.path)
            && talas::app::soundBrowserDrag(description) == talas::app::SoundBrowserDrag{sample.path, {}},
        "a sample dragged out should carry the file, as the File Organizer drags it");
    require(talas::app::soundBrowserDragDescription({"/library", {}, "library", 0, Kind::Folder}).empty()
            && talas::app::soundBrowserDragDescription({"/library/An Overview.talas.json", {}, "An Overview", 0, Kind::SoundFile}).empty(),
        "folders and Sound files should not be dragged");
    require(!talas::app::soundBrowserDrag(talas::app::talasFileDragDescription("/library/An Overview.talas.json")).has_value()
            && !talas::app::soundBrowserDrag("talas.browserSound:vocoder").has_value()
            && !talas::app::soundBrowserDrag("talas.prototype:talas.oscillator").has_value(),
        "a Sound file, a malformed Sound or a prototype should be no browser drag");
}

// A double-click on a
// folder spawns a browser, on a Sound file opens it, on a Sound brings it
// in to edit, on a sample or a spectrum opens its editor, on another file
// plays it.
void testDoubleClickingARow()
{
    using talas::app::SoundBrowserActivation;
    using talas::app::SoundBrowserKind;
    require(talas::app::soundBrowserActivation(SoundBrowserKind::Folder) == SoundBrowserActivation::Spawn
            && talas::app::soundBrowserActivation(SoundBrowserKind::SoundFile) == SoundBrowserActivation::OpenSoundFileWindow
            && talas::app::soundBrowserActivation(SoundBrowserKind::Sound) == SoundBrowserActivation::Edit,
        "a folder should spawn a browser, a Sound file open and a Sound come in to edit");
    // A Sound file's name double-clicked opens a Sound file
    // window of its Sounds as icons; its triangle opens it in the browser.
    const auto look = talas::app::soundBrowserRowLook({"/sounds/Pads.talas.json", {}, "Pads", 0, SoundBrowserKind::SoundFile});
    require(talas::app::soundBrowserActivation(SoundBrowserKind::SoundFile) == SoundBrowserActivation::OpenSoundFileWindow
            && look.triangle == talas::app::SoundBrowserTriangle::Sideways,
        "double-clicking a Sound file's name should open its Sound file window, the triangle still opening it in place");
    require(talas::app::soundBrowserActivation(SoundBrowserKind::Sample) == SoundBrowserActivation::OpenInEditor
            && talas::app::soundBrowserActivation(SoundBrowserKind::Spectrum) == SoundBrowserActivation::OpenInEditor,
        "a sample or a spectrum should open in its editor");
    require(talas::app::soundBrowserActivation(SoundBrowserKind::Midi) == SoundBrowserActivation::Play
            && talas::app::soundBrowserActivation(SoundBrowserKind::Ga) == SoundBrowserActivation::Play,
        "another file should play");
}

// The Timelines are an olive colour and have the
// file extension .talastimeline; such a file is a Sound file holding a Timeline,
// which Open any... opens and Play... plays.
void testTimelineFilesAreOlive()
{
    using talas::app::SoundBrowserKind;
    const auto root = std::filesystem::temp_directory_path() / "talas-sound-browser-timelines";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    talas::project::ProjectDocument haiku;
    haiku.metadata.title = "Haiku";
    std::ofstream{root / "Haiku.talastimeline"} << talas::project::serializeProjectDocument(haiku);
    std::ofstream{root / "Pads.talas.json"} << talas::project::serializeProjectDocument(haiku);
    const auto timeline = talas::app::soundBrowserKindOf(root / "Haiku.talastimeline");
    require(timeline == std::optional{SoundBrowserKind::SoundFile} && talas::app::isTimelineFile(root / "Haiku.TALASTIMELINE")
            && !talas::app::isTimelineFile(root / "Pads.talas.json"),
        "a .talastimeline file should be a Timeline's file, a Sound file holding it");
    talas::app::SoundBrowserRow row;
    row.path = root / "Haiku.talastimeline";
    row.kind = SoundBrowserKind::SoundFile;
    auto soundFile = row;
    soundFile.path = root / "Pads.talas.json";
    const auto olive = talas::app::soundBrowserRowLook(row).colour.value_or(0);
    const auto channel = [](std::uint32_t colour, int shift) { return static_cast<int>((colour >> shift) & 0xffu); };
    require(olive == talas::app::soundBrowserTimelineColour && channel(olive, 16) > 0x80 && channel(olive, 8) > 0x80
            && std::abs(channel(olive, 16) - channel(olive, 8)) < 0x10 && channel(olive, 0) < 0x60
            && talas::app::soundBrowserRowLook(soundFile).colour != talas::app::soundBrowserRowLook(row).colour,
        "the Timelines should be an olive colour, apart from the blue of other Sound files");
    const auto types = talas::app::talasFileTypes(false);
    const auto playable = talas::app::talasFileTypes(true);
    const auto listed = [](const auto& list) {
        return std::ranges::any_of(list, [](const auto& type) {
            return type.name == "Timeline" && talas::app::talasFileIsOfType("Haiku.talastimeline", type);
        });
    };
    require(listed(types) && listed(playable),
        "Open any... and Play... should take a Timeline's .talastimeline file");
    std::filesystem::remove_all(root);
}

int main()
{
    try
    {
        testDoubleClickingARow();
        testTimelineFilesAreOlive();
        testTheBrowserListsFoldersFilesAndSounds();
        testASpawnedBrowserOfOneFolder();
        testColouredBoxesShowAndHideKinds();
        testInformationOnTheSelectedRow();
        testFindingAName();
        testHowTheRowsLook();
        testExpressionsFilesListTheirExpressions();
        testTheReplacementInput();
        testPlayingASoundThroughTheReplacementInput();
        testTheGlobalInputReplacesTheReplaceableInput();
        testTheBrowsersSurfaceTakesChangesOncePlayedWithoutTheSubstituteInput();
        testTheSoundsAreListedAlphabetically();
        testDraggingRowsOutOfTheBrowser();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
