#include "talas/app/TalasKeyEquivalents.h"

#include <cctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The key equivalents of the workbench's menus.
namespace
{
using talas::app::TalasKeyCommand;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Ctrl+K kills the Sound playing.
void testKillingTheSound()
{
    const auto& kill = talas::app::talasKeyEquivalent(TalasKeyCommand::Stop);
    require(kill.key == 'k' && talas::app::talasKeyCommandFor('K') == TalasKeyCommand::Stop,
        "Ctrl+K stops the Sound playing");
}

// Ctrl+] zooms in, Ctrl+[ out, on the active display.
void testZoomingTheActiveDisplay()
{
    require(talas::app::talasKeyEquivalent(TalasKeyCommand::ZoomIn).key == ']'
            && talas::app::talasKeyEquivalent(TalasKeyCommand::ZoomOut).key == '['
            && talas::app::talasKeyEquivalent(TalasKeyCommand::ZoomIn).menu == "Edit",
        "Zoom in (Ctrl+]) and Zoom out (Ctrl+[) are in the Edit menu");
    require(talas::app::talasKeyCommandFor(']') == TalasKeyCommand::ZoomIn && talas::app::talasKeyCommandFor('[') == TalasKeyCommand::ZoomOut
            && !talas::app::talasKeyCommandFor('q'),
        "each bracket its zoom, another key none");
}

// Ctrl+B finds a prototype by part of its name.
void testFindingAPrototype()
{
    const auto& find = talas::app::talasKeyEquivalent(TalasKeyCommand::FindPrototype);
    require(find.key == 'b' && find.name == "Find Prototype..." && talas::app::talasKeyCommandFor('B') == TalasKeyCommand::FindPrototype,
        "Ctrl+B finds a prototype");
}

// Find... is Ctrl+F, Find again Ctrl+G and
// Replace... Ctrl+J, in the Edit menu.
void testFindingAndReplacingText()
{
    require(talas::app::talasKeyEquivalent(TalasKeyCommand::Find).key == 'f'
            && talas::app::talasKeyEquivalent(TalasKeyCommand::FindAgain).key == 'g'
            && talas::app::talasKeyEquivalent(TalasKeyCommand::Replace).key == 'j'
            && talas::app::talasKeyEquivalent(TalasKeyCommand::Replace).name == "Replace..."
            && talas::app::talasKeyEquivalent(TalasKeyCommand::Find).menu == "Edit",
        "Find... (Ctrl+F), Find again (Ctrl+G) and Replace... (Ctrl+J) are in the Edit menu");
}

// Expand is Ctrl+E.
void testExpandingASound()
{
    const auto& expand = talas::app::talasKeyEquivalent(TalasKeyCommand::Expand);
    require(expand.key == 'e' && expand.name == "Expand" && talas::app::talasKeyCommandFor('E') == TalasKeyCommand::Expand,
        "Ctrl+E expands the selected Sound");
}

// Ctrl+M sends MIDI notes off.
void testMidiNotesOff()
{
    const auto& off = talas::app::talasKeyEquivalent(TalasKeyCommand::MidiNotesOff);
    require(off.key == 'm' && off.name == "MIDI Notes Off" && talas::app::talasKeyCommandFor('M') == TalasKeyCommand::MidiNotesOff,
        "Ctrl+M sends MIDI notes off");
}

// File > Close (Ctrl+W) closes the active window.
void testClosingTheActiveWindow()
{
    const auto& close = talas::app::talasKeyEquivalent(TalasKeyCommand::Close);
    require(close.key == 'w' && close.menu == "File" && talas::app::talasKeyCommandFor('W') == TalasKeyCommand::Close,
        "Ctrl+W closes the window in front");
}

// A menu item's key is usually the first letter of its name,
// Ctrl+S for Save and Ctrl+O for Open... in the File menu.
void testTheKeyIsUsuallyTheNamesFirstLetter()
{
    const auto& save = talas::app::talasKeyEquivalent(TalasKeyCommand::Save);
    const auto& open = talas::app::talasKeyEquivalent(TalasKeyCommand::Open);
    require(save.key == 's' && save.menu == "File" && talas::app::talasKeyCommandFor('S') == TalasKeyCommand::Save
            && open.key == 'o' && open.menu == "File" && talas::app::talasKeyCommandFor('O') == TalasKeyCommand::Open,
        "Ctrl+S saves and Ctrl+O opens, from the File menu");
    // Each key is listed to the right of its item in a menu,
    // two keys, Ctrl (here Command) and a letter, but for the zooms'
    // brackets.
    for (const auto& equivalent : talas::app::talasKeyEquivalents())
    {
        require(!equivalent.menu.empty() && !equivalent.name.empty()
                && (std::isalpha(static_cast<unsigned char>(equivalent.key)) != 0 || equivalent.key == '['
                    || equivalent.key == ']'),
            "each key should be an item's in a menu, Command and one letter");
    }
}

// Ctrl+L opens the active field in a large window; Accept keeps what it
// holds, Cancel the field as it was.
void testTheLargeWindow()
{
    const auto& large = talas::app::talasKeyEquivalent(TalasKeyCommand::LargeWindow);
    require(large.key == 'l' && large.menu == "Edit" && large.name == "Large Window...",
        "Large window... (Ctrl+L) is in the Edit menu");
    const talas::app::LargeWindowField field{"| p | p := 3 c.", "| p | p := 4 c."};
    require(talas::app::largeWindowResult(field, true) == "| p | p := 4 c.", "Accept saves the window's contents into the field");
    require(talas::app::largeWindowResult(field, false) == "| p | p := 3 c.", "Cancel discards the changes");
}
}

// Space starts and pauses, Ctrl+Space
// rewinds and plays, Shift+Space plays the marked Sounds, Ctrl+Right Arrow
// plays and Ctrl+K stops.
void testTheTransportKeys()
{
    using talas::app::TalasTransportCommand;
    using talas::app::TalasTransportKeyName;
    const auto& pause = talas::app::talasTransportKey(TalasTransportCommand::PlayPause);
    require(pause.key == TalasTransportKeyName::Space && !pause.control && !pause.commandKey && !pause.shift,
        "Space alone starts and pauses");
    const auto& rewind = talas::app::talasTransportKey(TalasTransportCommand::RewindAndPlayAll);
    require(rewind.key == TalasTransportKeyName::Space && rewind.control, "Ctrl+Space rewinds and plays");
    const auto& marked = talas::app::talasTransportKey(TalasTransportCommand::PlayMarked);
    require(marked.key == TalasTransportKeyName::Space && marked.shift && !marked.control, "Shift+Space plays the marked Sounds");
    const auto& play = talas::app::talasTransportKey(TalasTransportCommand::Play);
    require(play.key == TalasTransportKeyName::RightArrow && play.commandKey, "Ctrl+Right Arrow plays");
    const auto& stop = talas::app::talasTransportKey(TalasTransportCommand::Stop);
    require(stop.key == TalasTransportKeyName::K && stop.commandKey
            && talas::app::talasKeyEquivalent(talas::app::TalasKeyCommand::Stop).key == 'k',
        "Ctrl+K stops, as the key equivalents have it");
    require(talas::app::talasTransportKeys().size() == 5, "five transport keys");
}

// The keys for play, pause, replay and stop, as
// they act in each state of the transport.
void testTheTransportKeysInEachState()
{
    using talas::app::TalasTransportAction;
    using talas::app::TalasTransportPress;
    using talas::app::TalasTransportState;
    const auto action = [](TalasTransportState state, TalasTransportPress press) { return talas::app::talasTransportAction(state, press); };
    require(action(TalasTransportState::Edited, TalasTransportPress::Space) == TalasTransportAction::CompileAndPlay
            && action(TalasTransportState::Edited, TalasTransportPress::ControlSpace) == TalasTransportAction::CompileAndPlay
            && action(TalasTransportState::Edited, TalasTransportPress::ControlR) == TalasTransportAction::ReplayLast
            && action(TalasTransportState::Edited, TalasTransportPress::ControlK) == TalasTransportAction::Stop,
        "after an edit or a new selection, Space and Ctrl+Space compile and play, Ctrl+R replays the last Sound");
    require(action(TalasTransportState::Playing, TalasTransportPress::Space) == TalasTransportAction::Pause
            && action(TalasTransportState::Playing, TalasTransportPress::ControlSpace) == TalasTransportAction::CompileAndPlay
            && action(TalasTransportState::Playing, TalasTransportPress::ControlR) == TalasTransportAction::ReplayThis
            && action(TalasTransportState::Playing, TalasTransportPress::ControlK) == TalasTransportAction::Stop,
        "playing, Space pauses and Ctrl+R replays this Sound");
    require(action(TalasTransportState::Paused, TalasTransportPress::Space) == TalasTransportAction::Resume
            && action(TalasTransportState::Paused, TalasTransportPress::ControlSpace) == TalasTransportAction::CompileAndPlay
            && action(TalasTransportState::Paused, TalasTransportPress::ControlR) == TalasTransportAction::ReplayThis
            && action(TalasTransportState::Paused, TalasTransportPress::ControlK) == TalasTransportAction::Stop,
        "paused, Space resumes and Ctrl+R replays this Sound");
    require(action(TalasTransportState::Stopped, TalasTransportPress::Space) == TalasTransportAction::ReplayLast
            && action(TalasTransportState::Stopped, TalasTransportPress::ControlSpace) == TalasTransportAction::CompileAndPlay
            && action(TalasTransportState::Stopped, TalasTransportPress::ControlR) == TalasTransportAction::ReplayLast
            && action(TalasTransportState::Stopped, TalasTransportPress::ControlK) == TalasTransportAction::Stop,
        "stopped, Space and Ctrl+R replay the last Sound played");
}

// The menu bar is File,
// Edit, DSP, Action, Info and Tools, then View and Help; Status, Stop,
// Restart and MIDI notes off are in the DSP menu; Compile, load, start,
// Compile & load, Record to disk, Expand, Revert, Find prototype and
// Evaluate in the Action menu; and Describe Sound, Get info, Structure as
// text, Full waveform, Oscilloscope, Spectrum analyzer and the environment
// in the Info menu.
void testTheMenus()
{
    using talas::app::TalasMenu;
    using Operation = talas::app::TalasMenuOperation;
    std::vector<std::string_view> names;
    for (std::size_t index = 0; index < talas::app::talasMenuCount; ++index)
        names.push_back(talas::app::talasMenuName(static_cast<TalasMenu>(index)));
    require(names == std::vector<std::string_view>{"File", "Edit", "DSP", "Action", "Info", "Tools", "View", "Help"},
        "the menus are File, Edit, DSP, Action, Info and Tools, then View and Help");
    require(talas::app::talasMenuPurpose(TalasMenu::Dsp).find("signal processor") != std::string_view::npos
            && talas::app::talasMenuPurpose(TalasMenu::Action).find("selected Sounds") != std::string_view::npos
            && talas::app::talasMenuPurpose(TalasMenu::Info).find("Information") != std::string_view::npos,
        "the DSP menu configures and monitors, the Action menu acts on and the Info menu informs on the selected Sounds");
    require(talas::app::talasMenuPurpose(TalasMenu::File).find("files") != std::string_view::npos
            && talas::app::talasMenuPurpose(TalasMenu::File).find("windows") != std::string_view::npos
            && talas::app::talasMenuPurpose(TalasMenu::Edit).find("active window") != std::string_view::npos
            && talas::app::talasMenuPurpose(TalasMenu::Tools).find("sound design") != std::string_view::npos,
        "the File menu makes and changes files and windows, Edit edits in the active window, Tools serve sound design");
    require(talas::app::talasMenuOf(Operation::Preferences) == TalasMenu::Edit, "Preferences is in the Edit menu");
    for (const auto operation : {Operation::TapeRecorder, Operation::SpectralAnalysis, Operation::ReAnalysis, Operation::GaAnalysis,
             Operation::SynchronizingSpectra, Operation::DesignAlternateTunings, Operation::FileArchivist,
             Operation::OnScreenKeyboard})
        require(talas::app::talasMenuOf(operation) == TalasMenu::Tools,
            "recording, the analyses, tunings, the archivist and the keyboard are Tools");
    require(talas::app::talasMenuOf(Operation::VirtualControlSurface) == TalasMenu::File,
        "Virtual control surface is in the File menu");
    for (const auto operation : {Operation::Status, Operation::Stop, Operation::Restart, Operation::MidiNotesOff,
             Operation::MidiMessages, Operation::ConfigureMidi, Operation::InitializeDsp, Operation::AudioSettings})
        require(talas::app::talasMenuOf(operation) == TalasMenu::Dsp,
            "the DSP menu's operations, Configure MIDI and Initialize DSP among them");
    // A problem with the signal processor shows a dialog
    // pointing to Initialize DSP.
    const auto problem = talas::app::dspProblemMessage("no audio device");
    require(problem.find("no audio device") != std::string::npos && problem.find("Initialize DSP from the DSP menu") != std::string::npos
            && problem.find("Click OK") != std::string::npos,
        "the warning should say to click OK and choose Initialize DSP from the DSP menu");
    // Expand warns that the original Sound cannot be
    // recovered once replaced by its expansion.
    require(talas::app::expandReplacementWarning().find("cannot recover the original Sound") != std::string::npos,
        "Expand should warn that the original Sound cannot be recovered");
    // Irreversible, Expand is better not done in the Sound
    // editor: a copy dragged into a Sound file window is expanded there.
    require(talas::app::expandReplacementWarning().find("drag a copy of the Sound into a Sound file window first")
            != std::string::npos,
        "the warning should suggest expanding a copy in a Sound file window");
    // Ctrl+Space plays from a parameter field, where Space
    // types a space.
    require(talas::app::parameterFieldTransportPress(true, true) == std::optional{talas::app::TalasTransportPress::ControlSpace}
            && !talas::app::parameterFieldTransportPress(true, false).has_value()
            && !talas::app::parameterFieldTransportPress(false, true).has_value(),
        "Ctrl+Space in a field should play, and Space type a space");
    // The Edit menu edits the active text field, the class
    // editor's description and comments in their own window too, else the
    // Sounds.
    require(talas::app::editMenuTarget(false, true) == talas::app::EditMenuTarget::TextField
            && talas::app::editMenuTarget(true, false) == talas::app::EditMenuTarget::TextField
            && talas::app::editMenuTarget(false, false) == talas::app::EditMenuTarget::Sounds,
        "the Edit menu should edit the active text field's text, in a tool window too");
    // Quit asks about each changed window, saves the
    // preferences, then exits.
    using talas::app::QuitStep;
    require(talas::app::quitSteps({true, true, true})
                == std::vector<QuitStep>{QuitStep::AskSampleEditor, QuitStep::AskClassEditor, QuitStep::AskProject,
                    QuitStep::SavePreferences, QuitStep::Exit}
            && talas::app::quitSteps({}) == std::vector<QuitStep>{QuitStep::SavePreferences, QuitStep::Exit},
        "Quit should ask about the changed windows, save the preferences and exit");
    // Closing while a parameter field holds a problem asks
    // first whether to drop the changes or continue editing.
    require(talas::app::quitSteps({false, false, false, true})
                == std::vector<QuitStep>{QuitStep::AskIllegalValue, QuitStep::SavePreferences, QuitStep::Exit},
        "closing with a problem in a parameter field should ask about it first");
    for (const auto operation : {Operation::CompileLoadStart, Operation::PlayMarked, Operation::PlayAll, Operation::Pause,
             Operation::CompileLoad, Operation::CompileToDisk, Operation::RecordToDisk, Operation::Expand, Operation::Revert, Operation::FindPrototype,
             Operation::Evaluate, Operation::NewClassFromExample, Operation::EditClass, Operation::RetrieveExampleFromClass,
             Operation::SetDefaultSound, Operation::SetDefaultCollection})
        require(talas::app::talasMenuOf(operation) == TalasMenu::Action, "the Action menu's operations");
    for (const auto operation : {Operation::DescribeSound, Operation::GetInfo, Operation::StructureAsText, Operation::FullWaveform,
             Operation::Oscilloscope, Operation::SpectrumAnalyzer, Operation::Environment, Operation::ResetEnvironment})
        require(talas::app::talasMenuOf(operation) == TalasMenu::Info, "the Info menu's operations");
    require(talas::app::talasKeyEquivalent(TalasKeyCommand::Stop).menu == "DSP"
            && talas::app::talasKeyEquivalent(TalasKeyCommand::MidiNotesOff).menu == "DSP"
            && talas::app::talasKeyEquivalent(TalasKeyCommand::Expand).menu == "Action"
            && talas::app::talasKeyEquivalent(TalasKeyCommand::FindPrototype).menu == "Action",
        "the keys' menus are the operations' menus");
}

// The operations that cannot apply where
// you are are dimmed: Revert outside the Sound editor or with no Sound
// selected, Environment outside the Sound editor, Find prototype without
// the prototypes.
void testOperationsOutOfContextAreDimmed()
{
    using Operation = talas::app::TalasMenuOperation;
    const talas::app::TalasMenuContext editing{true, true, true};
    const talas::app::TalasMenuContext nothingSelected{true, false, true};
    const talas::app::TalasMenuContext elsewhere{false, true, false};
    require(talas::app::talasMenuOperationApplies(Operation::Revert, editing)
            && !talas::app::talasMenuOperationApplies(Operation::Revert, nothingSelected)
            && !talas::app::talasMenuOperationApplies(Operation::Revert, elsewhere),
        "Revert applies only to a Sound selected in the Sound editor");
    require(talas::app::talasMenuOperationApplies(Operation::Environment, nothingSelected)
            && !talas::app::talasMenuOperationApplies(Operation::Environment, elsewhere),
        "Environment applies only in the Sound editor");
    require(talas::app::talasMenuOperationApplies(Operation::FindPrototype, editing)
            && !talas::app::talasMenuOperationApplies(Operation::FindPrototype, elsewhere),
        "Find prototype applies only where the prototypes are");
    require(talas::app::talasMenuOperationApplies(Operation::CompileLoadStart, elsewhere)
            && talas::app::talasMenuOperationApplies(Operation::Preferences, elsewhere),
        "operations that apply everywhere stay highlighted");
}

// New class from example
// applies only in a Sound file window. With a Sound editor in front, its
// Sound selected, or a Sound Browser in front, it is dimmed, and says to
// put the Sound in a Sound file window.
void testNewClassFromExampleOnlyInSoundFileWindows()
{
    using Operation = talas::app::TalasMenuOperation;
    talas::app::TalasMenuContext soundFile{false, false, true};
    soundFile.soundFileWindowInFront = true;
    const talas::app::TalasMenuContext soundEditor{true, true, true};
    const talas::app::TalasMenuContext soundBrowser{false, true, false};
    require(talas::app::talasMenuOperationApplies(Operation::NewClassFromExample, soundFile),
        "New class from example should apply in a Sound file window");
    require(!talas::app::talasMenuOperationApplies(Operation::NewClassFromExample, soundEditor)
            && !talas::app::talasMenuOperationApplies(Operation::NewClassFromExample, soundBrowser),
        "not with the Sound in a Sound editor or a Sound Browser");
    require(talas::app::talasMenuOperationNeeds(Operation::NewClassFromExample).find("Sound file window") != std::string_view::npos,
        "the dimmed command should say to put the Sound in a Sound file window");
    require(talas::app::talasMenuOperationApplies(Operation::EditClass, soundEditor)
            && talas::app::talasMenuOperationNeeds(Operation::EditClass).empty(),
        "Edit class stays where it was");
}

// Shift+click stops a recording to disk or a
// disk playback; a click without Shift, or with nothing on the disk, is
// left alone.
void testShiftClickStopsTheDisk()
{
    require(talas::app::shiftClickStopsDiskOperation(true, false, true) && talas::app::shiftClickStopsDiskOperation(false, true, true),
        "Shift+click should stop a recording to disk or a disk playback");
    require(!talas::app::shiftClickStopsDiskOperation(true, true, false) && !talas::app::shiftClickStopsDiskOperation(false, false, true),
        "a click without Shift, or with no disk operation, should be left alone");
}

// Update display during disk operations: on, Ctrl+K
// and the animated displays work while a Sound uses the disk; off, they
// wait and Shift+click stops the Sound; without the disk, both work.
void testUpdatingTheDisplayDuringDiskOperations()
{
    require(talas::app::interfaceRunsDuringDiskOperation(true, true) && !talas::app::interfaceRunsDuringDiskOperation(true, false)
            && talas::app::interfaceRunsDuringDiskOperation(false, false),
        "with the preference off, Ctrl+K and the displays should wait while a Sound uses the disk");
    require(talas::app::shiftClickStopsDiskOperation(false, true, true),
        "Shift+click should stop the disk playback even then");
    // The cursor while compiling, playing and
    // playing from the disk, and the plain arrow after.
    using talas::app::WorkbenchCursor;
    require(talas::app::workbenchCursor({false, true, false, false}) == WorkbenchCursor::Watch
            && talas::app::workbenchCursor({false, true, true, true}) == WorkbenchCursor::Watch
            && talas::app::workbenchCursor({false, false, true, false}) == WorkbenchCursor::Playing
            && talas::app::workbenchCursor({false, false, true, true}) == WorkbenchCursor::PlayingFromDisk
            && talas::app::workbenchCursor({false, false, true, true, false}) == WorkbenchCursor::WaitingForDisk
            && talas::app::workbenchCursor({true, true, true, false}) == WorkbenchCursor::Locating
            && talas::app::workbenchCursor({}) == WorkbenchCursor::Arrow && talas::app::workbenchCursor({false, false, false, true}) == WorkbenchCursor::Arrow,
        "a watch while compiling, a hollow arrow while playing, partly filled from the disk or a floppy disk "
        "waiting for it, then the arrow");
    // While samples load, an animated icon.
    require(talas::app::workbenchCursor({false, false, false, false, true, true}) == WorkbenchCursor::LoadingSamples
            && talas::app::workbenchCursor({false, true, true, false, true, true}) == WorkbenchCursor::LoadingSamples,
        "the cursor should turn into an animated icon while samples load");
    // A pen while an analysis is written to its file.
    talas::app::WorkbenchActivity writing;
    writing.writingFile = true;
    require(talas::app::workbenchCursor(writing) == WorkbenchCursor::WritingFile,
        "the cursor should change to the pen while it writes the file");
}

int main()
{
    try
    {
        testTheMenus();
        testOperationsOutOfContextAreDimmed();
        testNewClassFromExampleOnlyInSoundFileWindows();
        testShiftClickStopsTheDisk();
        testUpdatingTheDisplayDuringDiskOperations();
        testKillingTheSound();
        testZoomingTheActiveDisplay();
        testTheTransportKeysInEachState();
        testTheLargeWindow();
        testTheTransportKeys();
        testFindingAPrototype();
        testFindingAndReplacingText();
        testExpandingASound();
        testMidiNotesOff();
        testClosingTheActiveWindow();
        testTheKeyIsUsuallyTheNamesFirstLetter();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
