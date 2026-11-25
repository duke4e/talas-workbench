#include "talas/app/TalasFiles.h"
#include "talas/app/SelectedSoundDocument.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGlobalMap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::filesystem::path temporaryFolder()
{
    auto folder = std::filesystem::temp_directory_path() / ("talas-files-test-" + std::to_string(std::rand()));
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void little(std::vector<std::uint8_t>& bytes, std::uint32_t value, int count)
{
    for (int index = 0; index < count; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * index)));
}

// A mono 16-bit WAV of `frames` silent frames at 8 kHz.
std::vector<std::uint8_t> wav(std::uint32_t frames)
{
    std::vector<std::uint8_t> bytes{'R', 'I', 'F', 'F'};
    little(bytes, 36 + frames * 2, 4);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    little(bytes, 16, 4);
    little(bytes, 1, 2);
    little(bytes, 1, 2);
    little(bytes, 8000, 4);
    little(bytes, 16000, 4);
    little(bytes, 2, 2);
    little(bytes, 16, 2);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    little(bytes, frames * 2, 4);
    bytes.resize(bytes.size() + frames * 2, 0);
    return bytes;
}

// A Standard MIDI File of one track: a note on channel 1 and one on
// channel 3, a quarter note each at 120 beats a minute.
std::vector<std::uint8_t> midi()
{
    return {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0, 0, 20, 0, 0x90, 60, 100, 96, 0x80,
        60, 0, 0, 0x92, 64, 100, 96, 0x82, 64, 0, 0, 0xFF, 0x2F, 0};
}

// The kinds of file Talas uses and their
// colours in the organizer.
void testFileKindsAndColours()
{
    using Kind = talas::app::TalasFileKind;
    require(talas::app::talasFileKindOf("kick.wav") == Kind::Sample && talas::app::talasFileKindOf("voice EX.wav") == Kind::Sample
            && talas::app::talasFileKindOf("take.AIFF") == Kind::Sample && talas::app::talasFileKindOf("ircam.sf") == Kind::Sample
            && talas::app::talasFileKindOf("voice s48.talaspartial") == Kind::Spectrum
            && talas::app::talasFileKindOf("voice GA.wav") == Kind::Ga && talas::app::talasFileKindOf("voice RE.txt") == Kind::Re
            && talas::app::talasFileKindOf("tune.mid") == Kind::Midi && !talas::app::talasFileKindOf("notes.txt").has_value(),
        "samples, spectra, GA and RE analyses and MIDI files should be told apart");
    require(talas::app::talasFileKindColourName(Kind::Sample) == "turquoise" && talas::app::talasFileKindColourName(Kind::Spectrum) == "purple"
            && talas::app::talasFileKindColourName(Kind::Ga) == "red-brown" && talas::app::talasFileKindColourName(Kind::Re) == "yellow-orange"
            && talas::app::talasFileKindColourName(Kind::Midi) == "green",
        "each kind should have its colour");
    // A closed disk or folder gray, an open one underlined black.
    require(talas::app::fileOrganizerFolderLook(false).colour == 0xff808080u && !talas::app::fileOrganizerFolderLook(false).underlined
            && talas::app::fileOrganizerFolderLook(true).colour == 0xff000000u && talas::app::fileOrganizerFolderLook(true).underlined,
        "an unopened folder should be gray, an open one underlined black");
}

// The organizer lists the folders and
// the files Talas can use, indented beneath the folders opened by a
// double-click, and shows a file's information.
void testFileOrganizerListsFoldersAndInformation()
{
    const auto root = temporaryFolder();
    std::filesystem::create_directories(root / "drums");
    writeBytes(root / "kick.wav", wav(800));
    writeBytes(root / "drums" / "snare.wav", wav(400));
    writeBytes(root / "tune.mid", midi());
    writeBytes(root / "notes.txt", {'x'});
    talas::app::FileOrganizerModel organizer;
    organizer.setRoots({root});
    require(organizer.rows().size() == 1 && organizer.rows().front().folder && !organizer.rows().front().open,
        "a closed folder should list itself alone");
    organizer.toggle(root);
    auto rows = organizer.rows();
    require(rows.size() == 4 && rows[1].path.filename() == "drums" && rows[1].folder && rows[1].depth == 1
            && rows[2].path.filename() == "kick.wav" && rows[2].kind == talas::app::TalasFileKind::Sample
            && rows[3].path.filename() == "tune.mid" && rows[3].kind == talas::app::TalasFileKind::Midi,
        "an open folder should list its folders, then the files Talas uses, indented");
    organizer.toggle(root / "drums");
    rows = organizer.rows();
    require(rows.size() == 5 && rows[2].path.filename() == "snare.wav" && rows[2].depth == 2,
        "a folder opened beneath another should be indented further");
    organizer.toggle(root / "drums");
    require(organizer.rows().size() == 4, "a double-click should hide a folder's files again");

    const auto sample = talas::app::talasFileInformation(root / "kick.wav");
    require(sample.size() == 3 && sample[0] == (root / "kick.wav").string() && sample[1].starts_with("Modified ")
            && sample[2] == "1 channel · 8000 Hz · 800 frames · 0.100 s",
        "a sample's information should give its path, modification time and header");
    const auto tune = talas::app::talasFileInformation(root / "tune.mid");
    require(tune.size() == 3 && tune[2] == "1.000 s · channels 1, 3", "a MIDI file's information should give its length and channels");
    std::filesystem::remove_all(root);
}

// Icon Size in
// Sound Editor Fields shows an input field's Sounds as small or large
// icons, their names below, left to right and then top to bottom.
void testSoundFieldIconsAreSmallOrLarge()
{
    require(talas::app::soundFieldIconSize(true) > talas::app::soundFieldIconSize(false),
        "a large icon should be larger than a small one");
    const auto small = talas::app::soundFieldIconPlaces(5, 300, false);
    const auto large = talas::app::soundFieldIconPlaces(5, 300, true);
    require(small.size() == 5 && large.size() == 5 && small[0].height > talas::app::soundFieldIconSize(false)
            && large[0].height > talas::app::soundFieldIconSize(true),
        "an icon for each Sound, with room for its name below");
    const auto perRow = [](const std::vector<talas::app::SoundFieldIconPlace>& places) {
        std::size_t count = 0;
        while (count < places.size() && places[count].y == places.front().y)
            ++count;
        return count;
    };
    require(small[1].x > small[0].x && small[1].y == small[0].y && perRow(small) > perRow(large)
            && large[perRow(large)].x == 0 && large[perRow(large)].y > large[0].y,
        "left to right, then the next row from the left; more small icons to a row than large");
    require(talas::app::soundFieldIconPlaces(3, 10, false).back().y > 0, "a narrow field should still show its Sounds, one a row");
}

// The Spectrum
// Analyzer preferences set the length and window of the Info menu's
// spectrum analyzer only; a SpectrumAnalyzerDisplay Sound of one's own
// keeps the analysis panel's settings.
void testTheSpectrumAnalyzerPreferencesSetTheInfoAnalyzerOnly()
{
    const talas::app::SpectrumAnalyzerSettings preferred{64, talas::app::SpectrumAnalyzerWindow::Rectangular};
    const talas::app::SpectrumAnalyzerSettings panel{256, talas::app::SpectrumAnalyzerWindow::Blackman};
    require(talas::app::spectrumDisplaySettings(true, preferred, panel) == preferred,
        "the Info menu's analyzer should take the preferences' length and window");
    require(talas::app::spectrumDisplaySettings(false, preferred, panel) == panel,
        "a SpectrumAnalyzerDisplay Sound should not be affected by the preferences");
}

// The Spectrum Analyzer Window preference offers
// LinearEnvelope, Hamming, Hann and Long Kaiser, after the
// Rectangular and Blackman offered before, each kept in the preferences
// file by its name; a file naming the older three still reads them.
void testThePreferencesOfferTheManualsWindows()
{
    using Window = talas::app::SpectrumAnalyzerWindow;
    const auto& windows = talas::app::spectrumAnalyzerWindows();
    for (const auto manual : {Window::LinearEnvelope, Window::Hamming, Window::Hann, Window::LongKaiser})
        require(std::ranges::find(windows, manual) != windows.end(), "LinearEnvelope, Hamming, Hann and Long Kaiser should be offered");
    require(windows.size() == 6 && windows[0] == Window::Rectangular && windows[1] == Window::Hann && windows[2] == Window::Blackman,
        "the windows offered before keep their places");
    require(talas::app::spectrumAnalyzerWindowLabel(Window::LinearEnvelope) == "LinearEnvelope"
            && talas::app::spectrumAnalyzerWindowLabel(Window::LongKaiser) == "Long Kaiser",
        "the windows should go by their names, LinearEnvelope and Long Kaiser among them");
    for (const auto window : windows)
    {
        talas::app::WorkbenchPreferences preferences;
        preferences.spectrumAnalyzer = {64, window};
        require(talas::app::readWorkbenchPreferences(talas::app::writeWorkbenchPreferences(preferences)).spectrumAnalyzer
                == preferences.spectrumAnalyzer,
            "each window should be kept in the preferences file");
    }
    require(talas::app::readWorkbenchPreferences("talas-preferences 1\nspectrumAnalyzerWindow=blackman\n").spectrumAnalyzer.window
                == Window::Blackman,
        "a preferences file of before keeps its window");
}

// The Speaker Placement's first menu, Placement
// preset, fills in the speakers of a predefined setup, as Quad + Ctr +
// Sub's four corners, centre and subwoofer; a setup of one's own gives each
// speaker's radius and angle.
void testSpeakerPlacementPresetsFillTheSpeakers()
{
    const auto& presets = talas::project::speakerPlacementPresets();
    const auto quad = std::ranges::find(presets, std::string_view{"Quad + Ctr + Sub"}, &talas::project::SpeakerPlacementPreset::name);
    require(quad != presets.end() && quad->speakers.size() == 6 && quad->speakers.back().subwoofer
            && std::ranges::count_if(presets, [](const auto& preset) { return !preset.speakers.empty(); }) >= 4,
        "the predefined setups should include Quad + Ctr + Sub, a subwoofer among its speakers");
    const auto text = talas::project::speakerPlacementText(quad->speakers);
    require(text == "1 45\n1 135\n1 -45\n1 -135\n1 90\nsub" && talas::project::parseSpeakerPlacement(text) == quad->speakers
            && talas::project::speakerPlacementPresetName(quad->speakers) == std::optional<std::string_view>{"Quad + Ctr + Sub"},
        "choosing a predefined setup should fill in each of its speakers");
    const auto own = talas::project::parseSpeakerPlacement("1 30\n0.5 150\n");
    require(own.has_value() && own->size() == 2 && (*own)[1].radius == 0.5 && (*own)[1].angle == 150.0
            && !talas::project::speakerPlacementPresetName(*own).has_value(),
        "a setup of one's own should give each speaker's radius and angle");
    require(!talas::project::parseSpeakerPlacement("").has_value() && !talas::project::parseSpeakerPlacement("1").has_value()
            && !talas::project::parseSpeakerPlacement("1 400").has_value() && !talas::project::parseSpeakerPlacement("-1 10").has_value()
            && !talas::project::parseSpeakerPlacement("1 0\n1 0\n1 0\n1 0\n1 0\n1 0\n1 0\n1 0\n1 0").has_value(),
        "a placement should be one to eight speakers, each a radius of 0 or more and an angle within a turn");
}

// The preferences, kept in a file read
// at every start; a missing or damaged one leaves the defaults.
void testPreferencesRoundTrip()
{
    talas::app::WorkbenchPreferences preferences;
    require(!preferences.showFullPathNames && !preferences.showAllDigits && preferences.openVirtualControlsOnPlay
            && preferences.showHelpOnStartup && preferences.showUntitledOnStartup && preferences.showSoundBrowserOnStartup,
        "the defaults should open the virtual control surface on play and show help, an "
        "untitled window and the Sound Browser");
    preferences.showFullPathNames = true;
    preferences.showAllDigits = true;
    // Show Eight Channels in DSP Status.
    require(!preferences.showEightChannels, "the DSP Status shows two channels unless told to show eight");
    preferences.showEightChannels = true;
    // Sound Compiler Warnings, Help window only unless Notify
    // immediately is chosen.
    require(!preferences.notifyCompilerWarnings, "the compiler's warnings should show in the Help window only at first");
    preferences.notifyCompilerWarnings = true;
    // The System Output Level Pad, none unless
    // -6 dB is chosen for consumer gear.
    require(!preferences.padSystemOutput && talas::app::systemOutputGain(preferences) == 1.0F,
        "the outputs should be at full level unless padded");
    preferences.padSystemOutput = true;
    require(std::abs(20.0 * std::log10(talas::app::systemOutputGain(preferences)) + 6.0) < 1e-4,
        "the pad should take 6 dB off the outputs");
    preferences.openVirtualControlsOnPlay = false;
    preferences.showHelpOnStartup = false;
    preferences.showSoundBrowserOnStartup = false;
    // Update display during disk operations, on unless
    // switched off for a critical recording.
    require(preferences.updateDisplayDuringDiskOperations, "the display should update during disk operations by default");
    preferences.updateDisplayDuringDiskOperations = false;
    // Configure MIDI's Default Input Channel,
    // 1 unless chosen; a channel outside 1 to 16 is left.
    require(preferences.defaultMidiInputChannel == 1, "the Default Input Channel should be 1 at first");
    preferences.defaultMidiInputChannel = 16;
    require(talas::app::readWorkbenchPreferences("talas-preferences 1\ndefaultMidiInputChannel=17\n").defaultMidiInputChannel == 1
            && talas::app::readWorkbenchPreferences("talas-preferences 1\ndefaultMidiInputChannel=x\n").defaultMidiInputChannel == 1,
        "the Default Input Channel should be a channel from 1 to 16");
    preferences.frequentlyUsedFolders = {"/Samples", "/Spectra"};
    preferences.fileEditors[talas::app::TalasFileKind::Sample] = "/Applications/Wave Editor.app";
    // The number each prototype last gave a copy.
    preferences.prototypeCopyNumbers = {{"talas.oscillatorBank", 74}, {"talas.oscillator", 3}};
    // Allow stack traces, off unless checked.
    require(!preferences.allowStackTraces, "stack traces should be off at first");
    preferences.allowStackTraces = true;
    // Escape and one key name !KeyPitch unless the preferences ask for
    // !Pitch.
    require(!preferences.learnOneKeyAsPitch, "one key should name !KeyPitch at first");
    preferences.learnOneKeyAsPitch = true;
    // A selection in blue unless the preferences ask for gray.
    require(!preferences.graySelection, "a selection should be blue at first");
    preferences.graySelection = true;
    // Icon Size in Sound Editor Fields, small unless large
    // is chosen.
    require(!preferences.largeFieldIcons, "the Sounds in fields should show as small icons at first");
    preferences.largeFieldIcons = true;
    // The Speaker Placement, none (left and right) at first.
    require(preferences.speakerPlacement.empty(), "no speakers should be placed at first");
    preferences.speakerPlacement = {{1.0, 45.0}, {0.5, 135.0}, {1.0, 0.0, true}};
    // The Spectrum Analyzer's length and window.
    require(preferences.spectrumAnalyzer == talas::app::SpectrumAnalyzerSettings{256, talas::app::SpectrumAnalyzerWindow::Hann},
        "the spectrum analyzer should be 256 long with a Hann window at first");
    preferences.spectrumAnalyzer = {128, talas::app::SpectrumAnalyzerWindow::Blackman};
    // Configure MIDI's own Program Changes channel for the
    // surface, any channel at first; on it alone the presets change.
    require(preferences.vcsProgramChangeChannel == 0 && talas::app::programChangeSelectsVcsPreset(0, 9),
        "at first a program change on any channel should choose the surface's preset");
    preferences.vcsProgramChangeChannel = 5;
    require(talas::app::programChangeSelectsVcsPreset(5, 5) && !talas::app::programChangeSelectsVcsPreset(5, 1),
        "given its own channel, the surface should take that channel's program changes alone");
    const auto text = talas::app::writeWorkbenchPreferences(preferences);
    require(talas::app::readWorkbenchPreferences(text) == preferences, "preferences should be remembered through their file");
    require(talas::app::readWorkbenchPreferences("") == talas::app::WorkbenchPreferences{}
            && talas::app::readWorkbenchPreferences("garbage\nshowAllDigits=yes") == talas::app::WorkbenchPreferences{},
        "a missing or damaged file should give the defaults");
    require(talas::app::readWorkbenchPreferences("talas-preferences 1\nunknown=1\nshowAllDigits=maybe\n") == talas::app::WorkbenchPreferences{},
        "a line the preferences do not know should be skipped");
    // By default the Talas folder is a frequently used
    // folder; a file naming its own folders keeps only those.
    talas::app::WorkbenchPreferences defaults;
    defaults.frequentlyUsedFolders = {"/Users/me/Talas"};
    require(talas::app::readWorkbenchPreferences("", defaults).frequentlyUsedFolders == defaults.frequentlyUsedFolders
            && talas::app::readWorkbenchPreferences(text, defaults).frequentlyUsedFolders
                == std::vector<std::string>{"/Samples", "/Spectra"},
        "the Talas folder should be a frequently used folder by default");
    require(talas::app::displayedFileName("/Samples/kick.wav", preferences) == "/Samples/kick.wav"
            && talas::app::displayedFileName("/Samples/kick.wav", {}) == "kick.wav",
        "Show Full Path Names should show the whole path or the name alone");
    require(talas::app::displayedNumber(3.14159265358979, preferences) == "3.14159265359"
            && talas::app::displayedNumber(3.14159265358979, {}) == "3.14159",
        "Show All Digits should show twelve digits or six");
    // Caps Lock held at startup takes the defaults instead
    // of the file's preferences.
    talas::app::WorkbenchPreferences digits;
    digits.showAllDigits = true;
    const auto file = talas::app::writeWorkbenchPreferences(digits);
    require(talas::app::startupWorkbenchPreferences(file, {}, false).showAllDigits
            && talas::app::startupWorkbenchPreferences(file, {}, true) == talas::app::WorkbenchPreferences{},
        "starting with Caps Lock held should take the default preferences");
}

// A file opens in the external editor
// the preferences name for its kind, else in Talas's own; dragged into the
// Sound list it becomes a Sound that plays it.
void testFilesOpenAndBecomeSounds()
{
    using Kind = talas::app::TalasFileKind;
    using Opening = talas::app::TalasFileOpening;
    talas::app::WorkbenchPreferences preferences;
    require(talas::app::talasFileOpening(Kind::Sample, preferences) == Opening::SampleEditor
            && talas::app::talasFileOpening(Kind::Spectrum, preferences) == Opening::SpectrumEditor
            && talas::app::talasFileOpening(Kind::Midi, preferences) == Opening::MidiTimeline,
        "a file should open with the editor for its kind");
    preferences.fileEditors[Kind::Sample] = "/Applications/Wave Editor.app";
    require(talas::app::talasFileOpening(Kind::Sample, preferences) == Opening::ExternalEditor
            && talas::app::talasFileOpening(Kind::Midi, preferences) == Opening::MidiTimeline,
        "a kind checked for an external editor should open there");
    require(talas::app::talasFileOpening(Kind::Sample, preferences, true) == Opening::SampleEditor,
        "File > Open... always uses Talas's own editor");
    // An SD-II file's kind checked for an external editor
    // asks for the application the next time one is edited, and from then
    // on starts the application found.
    auto checked = talas::app::WorkbenchPreferences{};
    checked.fileEditors[Kind::Sample] = "";
    const auto reread = talas::app::readWorkbenchPreferences(talas::app::writeWorkbenchPreferences(checked));
    require(talas::app::talasFileKindOf("voice.sd2") == std::optional{Kind::Sample}
            && talas::app::talasFileOpening(Kind::Sample, reread) == Opening::AskForExternalEditor
            && talas::app::talasFileOpening(Kind::Spectrum, reread) == Opening::SpectrumEditor,
        "the next edit of a file of a kind checked should ask for the application");
    require(reread.fileEditors.size() == 1, "the checked kind should be kept");
    auto found = reread;
    found.fileEditors[Kind::Sample] = "/Applications/Wave Editor.app";
    require(talas::app::talasFileOpening(Kind::Sample,
                talas::app::readWorkbenchPreferences(talas::app::writeWorkbenchPreferences(found)))
            == Opening::ExternalEditor,
        "the application found should be remembered and started from then on");

    const auto root = temporaryFolder();
    writeBytes(root / "kick.wav", wav(800));
    writeBytes(root / "tune.mid", midi());
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry};
    const auto sample = talas::app::soundForTalasFile(editor.document(), root / "kick.wav");
    require(sample.has_value() && sample->resources.size() == 1 && sample->resources.front().file == root / "kick.wav",
        "a sample dragged in should become a Sound playing it");
    require(sample->definition.displayName == "kick" && sample->definition.graph.nodes.size() == 1
            && sample->definition.graph.nodes.front().typeId == "talas.genericSource"
            && sample->definition.graph.nodes.front().parameters.front().text == std::optional<std::string>{"Disk"},
        "a sample dragged in should become a GenericSource reading it from the disk, named for the file");
    require(editor.addSoundDefinition(sample->definition).ok(), "the sample's GenericSource should be one the project takes");
    const auto tune = talas::app::soundForTalasFile(editor.document(), root / "tune.mid");
    require(tune.has_value() && tune->resources.size() == 2
            && std::ranges::count(tune->definition.graph.nodes, std::string_view{"talas.midiVoice"}, &talas::project::ProjectNode::typeId) == 2
            && std::ranges::count(tune->definition.graph.nodes, std::string_view{"talas.mixerV2"}, &talas::project::ProjectNode::typeId) == 1,
        "a MIDI file should become a Mixer of a MIDIVoice for each channel");
    require(editor.addSoundDefinition(tune->definition).ok(), "the MIDI file's Sound should be one the project takes");
    require(!talas::app::soundForTalasFile(editor.document(), root / "notes.txt").has_value(), "other files should make no Sound");
    // A spectrum, as a region copied in the
    // spectrum editor is, becomes a Sound that resynthesizes it, a
    // partial-track player.
    talas::project::PartialTrackAsset spectrum;
    spectrum.sampleRate = 48000.0;
    spectrum.sourceFrameCount = 960;
    spectrum.frames = {{1, 0, {{1, 0.5, 440.0, 0.0}}}, {1, 480, {{1, 0.4, 440.0, 0.0}}}};
    const auto encoded = talas::project::encodePartialTrackAsset(spectrum);
    require(encoded.ok(), "the spectrum fixture should encode");
    writeBytes(root / "region.talaspartial", encoded.bytes);
    const auto resynthesis = talas::app::soundForTalasFile(editor.document(), root / "region.talaspartial");
    require(resynthesis.has_value() && resynthesis->resources.size() == 1
            && std::ranges::any_of(resynthesis->definition.graph.nodes,
                [](const talas::project::ProjectNode& node) { return node.typeId == "talas.partialTrackPlayerV1"; }),
        "a spectrum should become a Sound that plays it");
    std::filesystem::remove_all(root);
}

// Ctrl+Space in the File Organizer plays the
// selected file; a MIDI file through a minimal orchestra, a Sound file of
// its own whose Sound reads the file where it is.
void testOrganizerPlaysFilesThroughSounds()
{
    const auto root = temporaryFolder();
    writeBytes(root / "tune.mid", midi());
    const auto player = talas::app::talasFilePlayer(root / "tune.mid");
    require(player.has_value() && player->soundFile.soundDefinitions.size() == 1
            && player->soundFile.soundDefinitions.front().definitionId == player->definitionId && !player->name.empty(),
        "a MIDI file should be played by a Sound of its own");
    const auto& assets = player->soundFile.assets;
    require(assets.size() == 2
            && std::ranges::all_of(assets, [&](const auto& asset) {
                   return std::filesystem::path{asset.path} == (root / "tune.mid").lexically_normal()
                       && asset.kind == talas::project::ProjectAssetKind::Binary;
               }),
        "the orchestra's voices should read the MIDI file where it is");
    const auto& nodes = player->soundFile.soundDefinitions.front().graph.nodes;
    require(std::ranges::all_of(assets, [&](const auto& asset) {
                return std::ranges::any_of(nodes, [&](const talas::project::ProjectNode& node) {
                    return std::ranges::any_of(node.parameters, [&](const auto& parameter) { return parameter.resource == asset.id; });
                });
            }),
        "each voice's file field should name its file");
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry};
    require(editor.replaceDocument(player->soundFile).ok(), "the player's Sound file should be one the editor opens");
    // An analysis plays through the Sound that resynthesises it.
    writeBytes(root / "voice GA.wav", wav(800));
    const auto analysis = talas::app::talasFilePlayer(root / "voice GA.wav");
    require(analysis.has_value() && !analysis->soundFile.assets.empty()
            && std::filesystem::path{analysis->soundFile.assets.front().path} == (root / "voice GA.wav").lexically_normal()
            && std::ranges::any_of(analysis->soundFile.soundDefinitions.front().graph.nodes,
                [](const talas::project::ProjectNode& node) { return node.typeId == "talas.gaOscillators"; }),
        "a GA analysis should play through GA oscillators reading it");
    require(!talas::app::talasFilePlayer(root / "notes.txt").has_value(), "a file no Sound plays has no player");
    std::filesystem::remove_all(root);
}

// A file name dragged from the organizer into
// a parameter field sets it to the file's value where that makes sense: a
// sample's length in a duration field; nothing in a field it means nothing
// to.
void testFilesDraggedIntoFields()
{
    const auto root = temporaryFolder();
    writeBytes(root / "kick.wav", wav(800));
    const auto description = talas::app::talasFileDragDescription(root / "kick.wav");
    require(talas::app::talasFileFromDrag(description) == std::optional<std::filesystem::path>{root / "kick.wav"}
            && !talas::app::talasFileFromDrag("talasExpression:1").has_value(),
        "a drag from the organizer should carry the file and nothing else should pass for one");
    require(talas::app::talasFileFieldValue(root / "kick.wav", talas::expr::ValueDomain::Seconds) == std::optional<std::string>{"0.1 s"},
        "a sample dropped on a duration field gives its length");
    // A spectrum file dropped on a Duration field gives the
    // length of the sample it analyses.
    talas::project::PartialTrackAsset spectrum;
    spectrum.sampleRate = 48000.0;
    spectrum.sourceFrameCount = 96000;
    spectrum.frames = {{1, 0, {{1, 0.5, 440.0, 0.0}}}, {1, 480, {{1, 0.4, 440.0, 0.0}}}};
    const auto encoded = talas::project::encodePartialTrackAsset(spectrum);
    require(encoded.ok(), "the spectrum fixture should encode");
    writeBytes(root / "voice.talaspartial", encoded.bytes);
    require(talas::app::talasFileFieldValue(root / "voice.talaspartial", talas::expr::ValueDomain::Seconds)
            == std::optional<std::string>{"2 s"},
        "a spectrum dropped on a duration field gives the original's duration");
    require(!talas::app::talasFileFieldValue(root / "kick.wav", talas::expr::ValueDomain::Hertz).has_value()
            && !talas::app::talasFileFieldValue(root / "kick.wav", talas::expr::ValueDomain::Scalar).has_value(),
        "a field the file means nothing to is left as it was");
    std::filesystem::remove_all(root);
}

// Info > Full waveform records the selected Sound:
// it takes the output's place, its stereo outputs on both sides.
void testInfoTakesTheSelectedSound()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"noise", "talas.noise", {}}, {"pan", "talas.pan", {}}, {"gain", "talas.vca", {}}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {{"noise", "audioOut", "pan", "audioIn"}, {"pan", "leftOut", "gain", "audioIn"},
        {"gain", "audioOut", "out", "audioIn"}};
    const auto pan = talas::app::documentPlayingSound(document, registry, "pan");
    const auto into = [&](const talas::project::ProjectDocument& played, std::string_view port) {
        const auto found = std::ranges::find_if(played.graph.connections, [&](const auto& line) {
            return line.toNode == "out" && line.toPort == port;
        });
        return found == played.graph.connections.end() ? std::string{} : found->fromNode + "." + found->fromPort;
    };
    require(pan.has_value() && into(*pan, "audioIn") == "pan.leftOut" && into(*pan, "rightIn") == "pan.rightOut",
        "the selected stereo Sound should take the output's place on both sides");
    const auto noise = talas::app::documentPlayingSound(document, registry, "noise");
    require(noise.has_value() && into(*noise, "audioIn") == "noise.audioOut" && into(*noise, "rightIn").empty(),
        "a mono Sound should feed the output's left input");
    require(!talas::app::documentPlayingSound(document, registry, "ghost").has_value(), "a Sound not in the structure");

    // Oscilloscope and Spectrum analyzer play the selected Sound
    // through a display of it.
    const auto scope = talas::app::documentWithInfoDisplay(document, registry, "pan", talas::app::InfoDisplay::Oscilloscope);
    const auto from = [&](const talas::project::ProjectDocument& played, std::string_view node, std::string_view port) {
        const auto found = std::ranges::find_if(played.graph.connections, [&](const auto& line) {
            return line.toNode == node && line.toPort == port;
        });
        return found == played.graph.connections.end() ? std::string{} : found->fromNode + "." + found->fromPort;
    };
    const auto typeOf = [](const talas::project::ProjectDocument& played, std::string_view node) {
        const auto found = std::ranges::find(played.graph.nodes, node, &talas::project::ProjectNode::instanceId);
        return found == played.graph.nodes.end() ? std::string{} : found->typeId;
    };
    // Of a stereo Sound, the display shows a mix of its left
    // and right outputs, half of each, and the Sound is heard as it was,
    // the display joining the left at no gain.
    const auto gainOf = [](const talas::project::ProjectDocument& played, std::string_view node, std::string_view parameter) {
        const auto found = std::ranges::find(played.graph.nodes, node, &talas::project::ProjectNode::instanceId);
        if (found == played.graph.nodes.end())
            return std::string{};
        const auto gain = std::ranges::find(found->parameters, parameter, &talas::project::ProjectParameterExpression::parameterId);
        return gain == found->parameters.end() ? std::string{"1.0"} : gain->expression.value_or("");
    };
    const auto mixedDisplay = [&](const talas::app::InfoDisplayDocument& shown) {
        const auto& played = shown.document;
        const auto mix = from(played, shown.displayNodeId, "audioIn");
        const auto mixNode = mix.substr(0, mix.find('.'));
        const auto heard = into(played, "audioIn");
        const auto heardNode = heard.substr(0, heard.find('.'));
        return typeOf(played, mixNode) == "talas.mixer" && from(played, mixNode, "in1") == "pan.leftOut"
            && from(played, mixNode, "in2") == "pan.rightOut" && gainOf(played, mixNode, "gain1") == "0.5"
            && gainOf(played, mixNode, "gain2") == "0.5" && from(played, shown.displayNodeId, "rightIn").empty()
            && typeOf(played, heardNode) == "talas.mixer" && from(played, heardNode, "in1") == "pan.leftOut"
            && from(played, heardNode, "in2") == shown.displayNodeId + ".audioOut" && gainOf(played, heardNode, "gain2") == "0"
            && into(played, "rightIn") == "pan.rightOut";
    };
    require(scope.has_value() && typeOf(scope->document, scope->displayNodeId) == "talas.oscilloscopeDisplay" && mixedDisplay(*scope),
        "Oscilloscope should display a mix of the selected Sound's left and right outputs as it plays");
    const auto spectrum = talas::app::documentWithInfoDisplay(document, registry, "pan", talas::app::InfoDisplay::SpectrumAnalyzer);
    require(spectrum.has_value() && typeOf(spectrum->document, spectrum->displayNodeId) == "talas.spectrumAnalyzerDisplay"
            && mixedDisplay(*spectrum),
        "Spectrum analyzer should display the spectrum of a mix of the left and right outputs");
    // Ctrl+Space in the signal flow plays the one Sound selected.
    const auto alone = talas::app::documentForControlSpace(document, registry, {"pan"}, true);
    require(alone.has_value() && into(*alone, "audioIn") == "pan.leftOut" && into(*alone, "rightIn") == "pan.rightOut",
        "Ctrl+Space should compile, load and start the selected Sound alone");
    require(!talas::app::documentForControlSpace(document, registry, {"pan"}, false).has_value()
            && !talas::app::documentForControlSpace(document, registry, {"pan", "gain"}, true).has_value()
            && !talas::app::documentForControlSpace(document, registry, {}, true).has_value()
            && !talas::app::documentForControlSpace(document, registry, {"out"}, true).has_value(),
        "outside the signal flow, or with no Sound, several or the output selected, Ctrl+Space plays the whole project");
    // Full waveform suggests the length of the sample the
    // selected Sound plays, through the Sounds feeding it.
    auto sampled = document;
    sampled.assets.push_back({"bucket", talas::project::ProjectAssetKind::Audio, "bucket.wav"});
    sampled.assets.back().sampleRate = 48000.0;
    sampled.assets.back().frameCount = 72000;
    sampled.graph.nodes.front() = {"noise", "talas.sample", {}};
    sampled.graph.nodes.front().parameters.push_back({});
    sampled.graph.nodes.front().parameters.back().parameterId = "sample";
    sampled.graph.nodes.front().parameters.back().resource = "bucket";
    require(talas::app::selectedSoundSampleSeconds(sampled, "gain") == std::optional<double>{1.5}
            && !talas::app::selectedSoundSampleSeconds(document, "gain").has_value(),
        "Full waveform should suggest the length of the sample the Sound plays");
    // The Sound selected in the Sound file window, through a
    // display in the output's place.
    auto filed = document;
    filed.soundDefinitions.push_back({"hum", "Hum", {}, {{"audioOut", talas::core::PortType::Audio, "tone", "audioOut"}},
        {{{"tone", "talas.oscillator", {}}}, {}}});
    const auto hum = talas::app::soundWithInfoDisplay(filed, registry, "hum", talas::app::InfoDisplay::Oscilloscope);
    require(hum.has_value() && from(hum->document, hum->displayNodeId, "audioIn") == "soundFileSound.audioOut"
            && into(hum->document, "audioIn") == hum->displayNodeId + ".audioOut"
            && !talas::app::soundWithInfoDisplay(filed, registry, "ghost", talas::app::InfoDisplay::Oscilloscope).has_value(),
        "the Info menu should take the Sound selected in the Sound file window");
    auto named = document;
    named.graph.nodes.push_back({"infoDisplay", "talas.constant", {}});
    const auto renamed = talas::app::documentWithInfoDisplay(named, registry, "gain", talas::app::InfoDisplay::Oscilloscope);
    require(renamed.has_value() && renamed->displayNodeId == "infoDisplay2"
            && !talas::app::documentWithInfoDisplay(document, registry, "ghost", talas::app::InfoDisplay::Oscilloscope).has_value(),
        "the display should take a name no Sound has, and a Sound not in the structure shows nothing");
}
}

// A GenericSource whose Source is Ask
// asks for the source each time it plays, the recording on disk on Enter,
// the live input or a sample in RAM; the project keeps asking.
void testTheGenericSourceAsksForItsSource()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression ask;
    ask.parameterId = "source";
    ask.text = "Ask";
    document.graph.nodes = {{"voice", "talas.genericSource", {ask}}, {"tone", "talas.oscillator", {}}};
    talas::project::ProjectSoundDefinition sampler;
    sampler.definitionId = "sampler";
    sampler.graph.nodes = {{"input", "talas.genericSource", {ask}}};
    document.soundDefinitions.push_back(sampler);
    require(talas::app::genericSourcesAsking(document) == std::vector<std::string>{"voice", "sampler.input"},
        "the GenericSources whose Ask box is checked should ask");
    const auto sourceOf = [](const talas::project::ProjectNode& node) { return node.parameters.front().text.value_or(""); };
    const auto live = talas::app::documentWithGenericSourceAnswer(document, talas::app::GenericSourceAnswer::Live);
    const auto disk = talas::app::documentWithGenericSourceAnswer(document, talas::app::GenericSourceAnswer::Disk);
    require(sourceOf(live.graph.nodes.front()) == "Live" && sourceOf(live.soundDefinitions.front().graph.nodes.front()) == "Live"
            && sourceOf(disk.graph.nodes.front()) == "Disk" && talas::app::genericSourcesAsking(live).empty()
            && talas::app::genericSourceAnswerText(talas::app::GenericSourceAnswer::Ram) == "RAM"
            && sourceOf(document.graph.nodes.front()) == "Ask",
        "the answer, live input or the recording on disk, should play, the project still asking");
}

// Open any... lists every kind of file, Play...
// those it plays, each list showing files of the chosen type only; Ctrl+Up
// and Ctrl+Down bring each open window to the front in turn.
void testOpenAnyPlayAndChooseWindow()
{
    const auto all = talas::app::talasFileTypes(false);
    const auto playable = talas::app::talasFileTypes(true);
    require(all.size() > playable.size() && playable.front().name == "Sample file",
        "Play... lists the kinds of file it can play, Open any... every kind");
    require(talas::app::talasNewFileTypes()
            == std::vector<std::string>{"Sound file", "Timeline", "Tool", "Sample file", "Global map", "Text file",
                "Compiled Sound grid"},
        "New... should ask for the type of the new file from a list, a Compiled Sound grid among them");
    require(std::ranges::any_of(all, [](const auto& type) { return talas::app::talasFileIsOfType("Counter.talastool", type) && type.name == "Tool"; }),
        "a Tool saved on the disk should be among the files Open any... opens");
    // A Compiled Sound grid's .prg file opens, and New...
    // makes one, but it does not play.
    require(std::ranges::any_of(all, [](const auto& type) {
                return talas::app::talasFileIsOfType("Set A.prg", type) && type.name == "Compiled Sound grid";
            })
            && std::ranges::none_of(playable, [](const auto& type) { return talas::app::talasFileIsOfType("Set A.prg", type); }),
        "a Compiled Sound grid's file should open, not play");
    require(talas::app::talasFileIsOfType("voice.AIFF", playable.front()) && !talas::app::talasFileIsOfType("tune.mid", playable.front())
            && talas::app::talasFileIsOfType("Effects.talas.json", playable[1]),
        "the list shows files of the selected type only");
    // Spectra and MIDI files play, text files and global
    // maps do not.
    const auto plays = [&](std::string_view name) {
        return std::ranges::any_of(playable, [&](const auto& type) { return talas::app::talasFileIsOfType(name, type); });
    };
    require(plays("rai3 s256.spc") && plays("bell.talaspartial") && plays("tune.mid") && !plays("notes.txt") && !plays("Default.map"),
        "Play... should play spectrum and MIDI files, not text files or global maps");
    const auto opens = [&](std::string_view name) {
        return std::ranges::any_of(all, [&](const auto& type) { return talas::app::talasFileIsOfType(name, type); });
    };
    require(opens("rai3 s256.spc") && opens("notes.txt") && opens("Default.map"),
        "Open any... should open spectrum files, text files and global maps, which New... also makes");
    require(plays("Pad.talascompiled") && !opens("Pad.talascompiled"),
        "a compiled Sound file plays from Play... but does not open");
    require(talas::app::cycledWindowIndex(3, 2, 1) == 0 && talas::app::cycledWindowIndex(3, 0, -1) == 2
            && talas::app::cycledWindowIndex(3, 1, 1) == 2,
        "Ctrl+Down and Ctrl+Up cycle through the open windows");
}

// File > Open...
// takes a file type first; Edit Tool opens a Tool file in the Tool editor,
// Use Tool runs it, and Any openable file opens every kind in its editor.
void testOpenTakesTheEditToolFileType()
{
    const auto types = talas::app::talasOpenFileTypes();
    require(!types.empty() && types.front().name == "Sound file" && types.front().action == talas::app::TalasOpenAction::Project,
        "Open... should offer a Sound file first");
    const auto named = [&](std::string_view name) {
        const auto found = std::ranges::find(types, name, &talas::app::TalasOpenFileType::name);
        require(found != types.end(), "Open... should offer the file type " + std::string{name});
        return *found;
    };
    const auto lists = [](const talas::app::TalasOpenFileType& type, std::string_view file) {
        return talas::app::talasFileIsOfType(file, talas::app::TalasFileType{type.name, type.wildcard});
    };
    const auto edit = named("Edit Tool");
    require(edit.action == talas::app::TalasOpenAction::EditTool && lists(edit, "mouseToEventValue.talastool")
            && !lists(edit, "mouseToEvent Sounds.talas.json"),
        "the Edit Tool file type should list Tool files and open them in the Tool editor");
    const auto use = named("Use Tool");
    require(use.action == talas::app::TalasOpenAction::UseTool && lists(use, "mouseToEventValue.talastool"),
        "the Use Tool file type should run a Tool file");
    const auto any = named("Any openable file");
    require(any.action == talas::app::TalasOpenAction::AnyOpenableFile && lists(any, "mouseToEventValue.talastool")
            && lists(any, "notes.txt") && lists(any, "Set A.prg") && !lists(any, "Pad.talascompiled"),
        "Any openable file should list every kind Open any... opens");
    require(lists(named("Timeline"), "mouseToEvent timeline test.talastimeline") && !lists(named("Sound file"), "Counter.talastool"),
        "the list should show files of the selected type only");
}

// Text files open in the text file
// editor, which New... makes too; an RE analysis's table is the analysis.
void testTextFilesOpenInTheTextEditor()
{
    require(talas::app::talasFileOpensAsText("notes.txt") && talas::app::talasFileOpensAsText("sequences.TXT")
            && talas::app::talasFileOpensAsText("data/testExpressions.txt"),
        "a text file opens in the text file editor");
    require(!talas::app::talasFileOpensAsText("bell RE.txt") && !talas::app::talasFileOpensAsText("bell.aif")
            && !talas::app::talasFileOpensAsText("Sound.talas.json"),
        "an RE analysis, a sample or a Sound file is not opened as text");
    const auto types = talas::app::talasFileTypes(false);
    require(std::ranges::any_of(types, [](const auto& type) { return type.name == "Text file" && talas::app::talasFileIsOfType("a.txt", type); })
            && std::ranges::count(talas::app::talasNewFileTypes(), std::string{"Text file"}) == 1,
        "Open any... and New... take text files");
}

// A global map is
// a .map file Open... opens and Choose global map... makes the project's;
// the Global Maps folder holds Default, the Default map.
void testGlobalMapFiles()
{
    const auto types = talas::app::talasFileTypes(false);
    require(std::ranges::any_of(types, [](const auto& type) { return type.name == "Global map" && talas::app::talasFileIsOfType("Studio.map", type); })
            && talas::app::talasFileIsGlobalMap("Studio.MAP") && !talas::app::talasFileIsGlobalMap("Studio.txt"),
        "Open... should take a file of the Global map type");
    const auto root = temporaryFolder();
    const auto defaultMap = talas::app::defaultGlobalMapFile(root);
    const auto text = defaultMap.has_value() ? talas::app::globalMapFileText(*defaultMap) : std::nullopt;
    require(defaultMap == std::optional<std::filesystem::path>{root / "Global Maps" / "Default.map"} && text.has_value()
            && *text == talas::project::defaultGlobalMapText(),
        "the Global Maps folder should hold Default, the Default map");
    const auto studio = root / "Global Maps" / "Studio.map";
    {
        std::ofstream output(studio, std::ios::binary);
        output << "!Fader1 is: `MIDIController18.\n";
    }
    talas::project::ProjectEditor editor(talas::project::ProjectDocument{}, talas::core::makeBuiltinNodeRegistry());
    const auto chosen = talas::app::globalMapFileText(studio);
    require(chosen.has_value() && editor.setGlobalMap(*chosen).ok() && editor.document().globalMap == *chosen,
        "the map chosen should become the project's global map");
    require(talas::app::defaultGlobalMapFile(root) == defaultMap && !talas::app::globalMapFileText(root / "missing.map").has_value(),
        "Default should be written once, and a missing file reads nothing");
    std::filesystem::remove_all(root);
}

// View... sets the size of a Sound file window's
// icons, a Sound editor's icon size and connection style, and how the file
// organizer shows its folders.
void testViewChoices()
{
    using talas::app::ViewedWindow;
    const auto groups = [](ViewedWindow window) {
        std::set<std::string> found;
        for (const auto& choice : talas::app::viewChoices(window))
            found.insert(choice.group);
        return found;
    };
    require(groups(ViewedWindow::SoundFileWindow) == std::set<std::string>{"Icons"}
            && groups(ViewedWindow::SoundEditor) == std::set<std::string>{"Size", "Icons", "Connections", "Parameters", "Prototypes"}
            && groups(ViewedWindow::FileOrganizer) == std::set<std::string>{"Folders"},
        "View... should set a Sound file window's icons, a Sound editor's icons and connections, the organizer's folders");
    const auto small = talas::app::signalFlowIconScale(false);
    require(talas::app::signalFlowIconScale(true) == 1.0 && small * small == 0.25,
        "the signal flow's small icons should take a quarter of the area of large ones");
    const auto root = std::filesystem::temp_directory_path() / "talas-view-organizer-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "drums");
    std::ofstream{root / "bell.aif"} << "x";
    std::ofstream{root / "voice.aif"} << "x";
    talas::app::FileOrganizerModel organizer;
    organizer.setRoots({root});
    organizer.toggle(root);
    const auto names = [&] {
        std::vector<std::string> shown;
        for (const auto& row : organizer.rows())
            shown.push_back(row.path.filename().string());
        return shown;
    };
    const auto first = names();
    organizer.setFoldersFirst(false);
    const auto mixed = names();
    std::filesystem::remove_all(root);
    require(first == std::vector<std::string>{"talas-view-organizer-test", "drums", "bell.aif", "voice.aif"}
            && mixed == std::vector<std::string>{"talas-view-organizer-test", "bell.aif", "drums", "voice.aif"},
        "the organizer should show folders first, or among the files by name");
}

// Tools placed in the Tools folder are on the
// Tools menu.
void testToolsInTheToolsFolder()
{
    const auto root = std::filesystem::temp_directory_path() / "talas-tools-folder-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Tools");
    std::filesystem::create_directories(root / "Other");
    for (const auto& name : {"Tools/widgetSampler.talastool", "Tools/Counter.talastool", "Tools/notes.txt", "Other/elsewhere.talastool"})
        std::ofstream{root / name} << "{}";
    const auto tools = talas::app::toolsInToolsFolders({root, root / "Tools"});
    std::filesystem::remove_all(root);
    require(tools.size() == 2 && tools[0].filename() == "Counter.talastool" && tools[1].filename() == "widgetSampler.talastool",
        "the Tool files in the Tools folder should be on the Tools menu, by name, each once");
}

// File > Documentation lists the PDFs (and Markdown
// and HTML files) in the Documentation folders, by name.
void testTheDocumentationFolder()
{
    const auto root = std::filesystem::temp_directory_path() / "talas-documentation-folder-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Documentation");
    for (const auto& name : {"Documentation/Guide.pdf", "Documentation/Getting Started.md",
             "Documentation/Prototypes.PDF", "Documentation/notes.txt", "Elsewhere.pdf"})
        std::ofstream{root / name} << "%";
    const auto documents = talas::app::documentationInDocumentationFolders({root, root / "Documentation"});
    std::filesystem::remove_all(root);
    require(documents.size() == 3 && documents[0].filename() == "Getting Started.md"
            && documents[1].filename() == "Guide.pdf" && documents[2].filename() == "Prototypes.PDF",
        "File > Documentation should list the Documentation folder's PDFs, by name, each once");
}

// Open lists the folders
// inside one, Add puts the one selected into the frequently used folders,
// once, and Remove takes one out.
void testFrequentlyUsedFoldersAreAddedAndRemoved()
{
    const auto root = temporaryFolder();
    std::filesystem::create_directories(root / "Samples" / "Drums");
    std::filesystem::create_directories(root / "analyses");
    std::filesystem::create_directories(root / ".hidden");
    std::ofstream{root / "note.txt"} << "not a folder";
    const auto inside = talas::app::foldersInside(root);
    require(inside == std::vector<std::filesystem::path>{root / "analyses", root / "Samples"},
        "Open should list the folders inside, by name, files and hidden folders left out");
    require(talas::app::foldersInside(root / "Samples") == std::vector<std::filesystem::path>{root / "Samples" / "Drums"},
        "Open on a folder inside should show its own folders");
    talas::app::WorkbenchPreferences preferences;
    preferences = talas::app::withFrequentlyUsedFolder(preferences, root / "Samples");
    preferences = talas::app::withFrequentlyUsedFolder(preferences, root / "analyses");
    preferences = talas::app::withFrequentlyUsedFolder(preferences, root / "Samples");
    require(preferences.frequentlyUsedFolders
                == std::vector<std::string>{(root / "Samples").string(), (root / "analyses").string()},
        "Add should put the folder at the end of the list, once");
    preferences = talas::app::withoutFrequentlyUsedFolder(preferences, 0);
    require(preferences.frequentlyUsedFolders == std::vector<std::string>{(root / "analyses").string()}
            && talas::app::withoutFrequentlyUsedFolder(preferences, 5) == preferences,
        "Remove should take the folder selected out of the list");
    std::filesystem::remove_all(root);
}

// The release number and date in Preferences.
void testTheReleaseInPreferences()
{
    require(talas::app::preferencesReleaseLine("0.1.0", "Nov  2 2026") == "Talas 0.1.0, released 2026-11-02"
            && talas::app::preferencesReleaseLine("1.4.2", "Jan 15 2027") == "Talas 1.4.2, released 2027-01-15",
        "Preferences should give the release number and its date");
    require(talas::app::preferencesReleaseLine("0.1.0", "someday") == "Talas 0.1.0"
            && talas::app::preferencesReleaseLine("0.1.0", "Foo  2 2026") == "Talas 0.1.0",
        "a date that cannot be read should leave the release number alone");
}

// A DiskRecorder records into the file its FileName
// names; with no full path it goes into the Program folder of the Talas
// folder, and a name without an extension is a WAV file.
void testADiskRecorderRecordsIntoItsFileName()
{
    const std::filesystem::path program = std::filesystem::path{"/Users/me/Talas"} / "Program";
    require(talas::app::diskRecorderFile("dry take", program) == program / "dry take.wav"
            && talas::app::diskRecorderFile("takes/dry.wav", program) == program / "takes" / "dry.wav",
        "a file name without a full path should go into the Program folder");
    require(talas::app::diskRecorderFile("/Volumes/Audio/dry.wav", program) == std::filesystem::path{"/Volumes/Audio/dry.wav"},
        "a full path name should record there");
    require(talas::app::diskRecorderFile("dry take", program, true) == program / "dry take.aif",
        "a DiskRecorder whose Format is AIFF should record an AIFF file");
    // Each Format its own extension.
    require(talas::app::diskRecorderFileOfFormat("dry take", program, ".sd1") == program / "dry take.sd1"
            && talas::app::diskRecorderFileOfFormat("dry take", program, ".sf") == program / "dry take.sf"
            && talas::app::diskRecorderFileOfFormat("dry.sd2", program, ".sd2") == program / "dry.sd2",
        "a DiskRecorder whose Format is SDI, IRCAM or SDII should record a file of that format");
}

// SD-I (.sd1) and SD-II (.sd2) sample
// files open, as sample files, and play: they are samples to the File
// Organizer, and Open any... and Play... list them among the sample files.
void testSoundDesignerFilesOpenAndPlay()
{
    const auto playable = talas::app::talasFileTypes(true);
    const auto all = talas::app::talasFileTypes(false);
    for (const auto* name : {"voice.sd1", "voice.SD2", "voice.sf"})
    {
        require(talas::app::talasFileKindOf(name) == std::optional<talas::app::TalasFileKind>{talas::app::TalasFileKind::Sample},
            std::string{""} + name + " should be a sample file");
        require(talas::app::talasFileIsOfType(name, playable.front()) && playable.front().name == "Sample file"
                && std::ranges::any_of(all, [&](const auto& type) { return talas::app::talasFileIsOfType(name, type); }),
            std::string{"Play... and Open any... should list "} + name);
    }
}

// A LIME file is not opened, created or
// played: neither Open any... nor Play... lists one, New... does not make
// one, and it is no sample, spectrum or MIDI file (the Import column,
// setting a Macintosh file type from the name, has no counterpart here).
void testLimeFilesAreNotOpenedCreatedOrPlayed()
{
    for (const auto playableOnly : {false, true})
    {
        require(std::ranges::none_of(talas::app::talasFileTypes(playableOnly),
                    [](const auto& type) { return talas::app::talasFileIsOfType("melody.lim", type); }),
            "a LIME file should not be listed by Open any... or Play...");
    }
    require(std::ranges::none_of(talas::app::talasNewFileTypes(),
                [](const auto& type) { return type.find("LIME") != std::string::npos || type.find("Lime") != std::string::npos; })
            && !talas::app::talasFileKindOf("melody.lim").has_value(),
        "New... should not make a LIME file, and a LIME file is no file Talas plays");
}

int main()
{
    try
    {
        testFileKindsAndColours();
        testADiskRecorderRecordsIntoItsFileName();
        testSoundDesignerFilesOpenAndPlay();
        testLimeFilesAreNotOpenedCreatedOrPlayed();
        testTheReleaseInPreferences();
        testFileOrganizerListsFoldersAndInformation();
        testPreferencesRoundTrip();
        testSoundFieldIconsAreSmallOrLarge();
        testSpeakerPlacementPresetsFillTheSpeakers();
        testTheSpectrumAnalyzerPreferencesSetTheInfoAnalyzerOnly();
        testThePreferencesOfferTheManualsWindows();
        testFrequentlyUsedFoldersAreAddedAndRemoved();
        testFilesOpenAndBecomeSounds();
        testOrganizerPlaysFilesThroughSounds();
        testFilesDraggedIntoFields();
        testInfoTakesTheSelectedSound();
        testTheGenericSourceAsksForItsSource();
        testOpenAnyPlayAndChooseWindow();
        testOpenTakesTheEditToolFileType();
        testToolsInTheToolsFolder();
        testTheDocumentationFolder();
        testViewChoices();
        testTextFilesOpenInTheTextEditor();
        testGlobalMapFiles();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Talas files tests passed\n";
    return 0;
}
