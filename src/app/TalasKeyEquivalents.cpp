#include "talas/app/TalasKeyEquivalents.h"

#include <algorithm>
#include <array>

namespace talas::app
{
namespace
{
constexpr std::array equivalents{
    TalasKeyEquivalent{TalasKeyCommand::Stop, 'k', "Stop", "DSP",
        "Stop (kill) the Sound playing and return to the start"},
    TalasKeyEquivalent{TalasKeyCommand::ZoomIn, ']', "Zoom In", "Edit",
        "Magnify the active display: the field's graphic editor, or the signal flow"},
    TalasKeyEquivalent{TalasKeyCommand::ZoomOut, '[', "Zoom Out", "Edit",
        "Show more of the active display: the field's graphic editor, or the signal flow"},
    TalasKeyEquivalent{TalasKeyCommand::LargeWindow, 'l', "Large Window...", "Edit",
        "Open the active parameter field in a window as large as the screen"},
    // Ctrl+B finds a prototype by part of its name.
    TalasKeyEquivalent{TalasKeyCommand::FindPrototype, 'b', "Find Prototype...", "Action",
        "Open the Library and search the prototypes by part of a name"},
    // Find... (Ctrl+F), Find again (Ctrl+G) and Replace...
    // (Ctrl+J) in the active text field.
    TalasKeyEquivalent{TalasKeyCommand::Find, 'f', "Find...", "Edit", "Search the active text field after the selection"},
    TalasKeyEquivalent{TalasKeyCommand::FindAgain, 'g', "Find Again", "Edit",
        "Search the active text field again for the last text found"},
    TalasKeyEquivalent{TalasKeyCommand::Replace, 'j', "Replace...", "Edit",
        "Replace the next occurrence of a text in the active field, or all of them"},
    // Expand (Ctrl+E), a copy expanded in the
    // Sound file window, the selected Sound replaced in a Sound editor.
    TalasKeyEquivalent{TalasKeyCommand::Expand, 'e', "Expand", "Action",
        "Replace the selected Sound with its Sounds, one level; in the Sound file window, add an expanded copy"},
    // MIDI notes off (Ctrl+M) in the DSP menu.
    TalasKeyEquivalent{TalasKeyCommand::MidiNotesOff, 'm', "MIDI Notes Off", "DSP",
        "Send a note-off for every note on every MIDI channel, releasing any note left hanging"},
    // File > Close closes the active window.
    TalasKeyEquivalent{TalasKeyCommand::Close, 'w', "Close", "File", "Close the window in front, as its close box does"},
    // A menu item's key is usually its name's first letter,
    // Ctrl+S for Save... and Ctrl+O for Open....
    TalasKeyEquivalent{TalasKeyCommand::Open, 'o', "Open...", "File", "Open a file of the type chosen"},
    TalasKeyEquivalent{TalasKeyCommand::Save, 's', "Save", "File", "Save the current project"},
};
}

constexpr std::array transportKeys{
    TalasTransportKey{TalasTransportCommand::PlayPause, TalasTransportKeyName::Space, false, false, false,
        "Play the Timeline from the time cursor, or pause it"},
    TalasTransportKey{TalasTransportCommand::RewindAndPlayAll, TalasTransportKeyName::Space, true, false, false,
        "Return to the start, compile and play every Sound"},
    TalasTransportKey{TalasTransportCommand::PlayMarked, TalasTransportKeyName::Space, false, false, true,
        "Compile and play the marked Sounds alone"},
    TalasTransportKey{TalasTransportCommand::Play, TalasTransportKeyName::RightArrow, false, true, false,
        "Play, Space alone pausing"},
    TalasTransportKey{TalasTransportCommand::Stop, TalasTransportKeyName::K, false, true, false,
        "Stop (kill) and return to the start"},
};

std::span<const TalasTransportKey> talasTransportKeys() noexcept
{
    return transportKeys;
}

const TalasTransportKey& talasTransportKey(TalasTransportCommand command) noexcept
{
    return *std::ranges::find(transportKeys, command, &TalasTransportKey::command);
}

std::span<const TalasKeyEquivalent> talasKeyEquivalents() noexcept
{
    return equivalents;
}

const TalasKeyEquivalent& talasKeyEquivalent(TalasKeyCommand command) noexcept
{
    return *std::ranges::find(equivalents, command, &TalasKeyEquivalent::command);
}

std::optional<TalasKeyCommand> talasKeyCommandFor(char key) noexcept
{
    if (key >= 'A' && key <= 'Z')
        key = static_cast<char>(key - 'A' + 'a');
    const auto found = std::ranges::find(equivalents, key, &TalasKeyEquivalent::key);
    if (found == equivalents.end())
        return std::nullopt;
    return found->command;
}

std::string largeWindowResult(const LargeWindowField& field, bool accepted)
{
    return accepted ? field.edited : field.original;
}

TalasTransportAction talasTransportAction(TalasTransportState state, TalasTransportPress press) noexcept
{
    switch (press)
    {
    case TalasTransportPress::ControlSpace:
        return TalasTransportAction::CompileAndPlay;
    case TalasTransportPress::ControlK:
        return TalasTransportAction::Stop;
    case TalasTransportPress::ControlR:
        return state == TalasTransportState::Playing || state == TalasTransportState::Paused ? TalasTransportAction::ReplayThis
                                                                                           : TalasTransportAction::ReplayLast;
    case TalasTransportPress::Space:
        break;
    }
    switch (state)
    {
    case TalasTransportState::Edited:
        return TalasTransportAction::CompileAndPlay;
    case TalasTransportState::Playing:
        return TalasTransportAction::Pause;
    case TalasTransportState::Paused:
        return TalasTransportAction::Resume;
    case TalasTransportState::Stopped:
        break;
    }
    return TalasTransportAction::ReplayLast;
}

std::string_view talasMenuName(TalasMenu menu) noexcept
{
    switch (menu)
    {
    case TalasMenu::File: return "File";
    case TalasMenu::Edit: return "Edit";
    case TalasMenu::Dsp: return "DSP";
    case TalasMenu::Action: return "Action";
    case TalasMenu::Info: return "Info";
    case TalasMenu::Tools: return "Tools";
    case TalasMenu::View: return "View";
    case TalasMenu::Help: return "Help";
    }
    return "File";
}

std::string_view talasMenuPurpose(TalasMenu menu) noexcept
{
    switch (menu)
    {
    case TalasMenu::File: return "Create or modify files, file editors and other windows";
    case TalasMenu::Edit: return "Edit Sounds, samples or text in the active window";
    case TalasMenu::Dsp: return "Configure and monitor the signal processor";
    case TalasMenu::Action: return "Act on the selected Sounds in the active window";
    case TalasMenu::Info: return "Information on the selected Sounds in the active window";
    case TalasMenu::Tools: return "Tools for sound design and development";
    case TalasMenu::View: return "The workspaces, panes and signal flow's presentation";
    case TalasMenu::Help: return "Help on Talas and its Sounds";
    }
    return {};
}

TalasMenu talasMenuOf(TalasMenuOperation operation) noexcept
{
    switch (operation)
    {
    case TalasMenuOperation::VirtualControlSurface: return TalasMenu::File;
    case TalasMenuOperation::Status:
    case TalasMenuOperation::Stop:
    case TalasMenuOperation::Restart:
    case TalasMenuOperation::MidiNotesOff:
    case TalasMenuOperation::MidiMessages:
    case TalasMenuOperation::ConfigureMidi:
    case TalasMenuOperation::InitializeDsp:
    case TalasMenuOperation::AudioSettings: return TalasMenu::Dsp;
    case TalasMenuOperation::CompileLoadStart:
    case TalasMenuOperation::PlayMarked:
    case TalasMenuOperation::PlayAll:
    case TalasMenuOperation::Pause:
    case TalasMenuOperation::CompileLoad:
    case TalasMenuOperation::CompileToDisk:
    case TalasMenuOperation::RecordToDisk:
    case TalasMenuOperation::Expand:
    case TalasMenuOperation::Revert:
    case TalasMenuOperation::FindPrototype:
    case TalasMenuOperation::Evaluate:
    case TalasMenuOperation::NewClassFromExample:
    case TalasMenuOperation::EditClass:
    case TalasMenuOperation::RetrieveExampleFromClass:
    case TalasMenuOperation::SetDefaultSound:
    case TalasMenuOperation::SetDefaultCollection: return TalasMenu::Action;
    case TalasMenuOperation::DescribeSound:
    case TalasMenuOperation::GetInfo:
    case TalasMenuOperation::StructureAsText:
    case TalasMenuOperation::FullWaveform:
    case TalasMenuOperation::Oscilloscope:
    case TalasMenuOperation::SpectrumAnalyzer:
    case TalasMenuOperation::Environment:
    case TalasMenuOperation::ResetEnvironment: return TalasMenu::Info;
    case TalasMenuOperation::Preferences: return TalasMenu::Edit;
    case TalasMenuOperation::TapeRecorder:
    case TalasMenuOperation::SpectralAnalysis:
    case TalasMenuOperation::ReAnalysis:
    case TalasMenuOperation::GaAnalysis:
    case TalasMenuOperation::SynchronizingSpectra:
    case TalasMenuOperation::DesignAlternateTunings:
    case TalasMenuOperation::FileArchivist:
    case TalasMenuOperation::OnScreenKeyboard: return TalasMenu::Tools;
    }
    return TalasMenu::Tools;
}

bool talasMenuOperationApplies(TalasMenuOperation operation, const TalasMenuContext& context) noexcept
{
    switch (operation)
    {
    case TalasMenuOperation::Revert:
        return context.soundEditorShown && context.soundSelected;
    case TalasMenuOperation::Environment:
        return context.soundEditorShown;
    case TalasMenuOperation::FindPrototype:
        return context.prototypesShown;
    // Only in a Sound file window.
    case TalasMenuOperation::NewClassFromExample:
        return context.soundFileWindowInFront;
    default:
        return true;
    }
}

std::string_view talasMenuOperationNeeds(TalasMenuOperation operation) noexcept
{
    switch (operation)
    {
    case TalasMenuOperation::Revert:
        return "Select a Sound in the Sound editor";
    case TalasMenuOperation::Environment:
        return "Bring a Sound editor to the front";
    case TalasMenuOperation::FindPrototype:
        return "Show the prototypes";
    case TalasMenuOperation::NewClassFromExample:
        return "Put the Sound in a Sound file window and select it there, not in a Sound editor or a Sound Browser";
    default:
        return {};
    }
}

bool shiftClickStopsDiskOperation(bool recordingToDisk, bool diskSoundPlaying, bool shiftHeld) noexcept
{
    return shiftHeld && (recordingToDisk || diskSoundPlaying);
}

bool interfaceRunsDuringDiskOperation(bool diskSoundPlaying, bool updateDisplayDuringDiskOperations) noexcept
{
    return !diskSoundPlaying || updateDisplayDuringDiskOperations;
}

WorkbenchCursor workbenchCursor(const WorkbenchActivity& activity) noexcept
{
    if (activity.locatingFile)
        return WorkbenchCursor::Locating;
    if (activity.writingFile)
        return WorkbenchCursor::WritingFile;
    if (activity.loadingSamples)
        return WorkbenchCursor::LoadingSamples;
    if (activity.compiling)
        return WorkbenchCursor::Watch;
    if (!activity.playing)
        return WorkbenchCursor::Arrow;
    if (!activity.usingDisk)
        return WorkbenchCursor::Playing;
    return activity.updateDisplayDuringDiskOperations ? WorkbenchCursor::PlayingFromDisk : WorkbenchCursor::WaitingForDisk;
}

std::string dspProblemMessage(std::string_view reason)
{
    return "The signal processor is not answering" + (reason.empty() ? std::string{} : " (" + std::string{reason} + ")")
        + ". Click OK, then choose Initialize DSP from the DSP menu.";
}

std::string expandReplacementWarning()
{
    // Expand is irreversible, so it is best done on
    // a copy dragged into a Sound file window, whose Expand asks nothing.
    return "You cannot recover the original Sound if you replace it with its expansion. "
           "To keep it, drag a copy of the Sound into a Sound file window first and expand the copy there.";
}

std::optional<TalasTransportPress> parameterFieldTransportPress(bool space, bool control) noexcept
{
    if (space && control)
        return TalasTransportPress::ControlSpace;
    return std::nullopt;
}

EditMenuTarget editMenuTarget(bool inSoundEditor, bool inToolWindow) noexcept
{
    return inSoundEditor || inToolWindow ? EditMenuTarget::TextField : EditMenuTarget::Sounds;
}

std::vector<QuitStep> quitSteps(const QuitState& state)
{
    std::vector<QuitStep> steps;
    if (state.illegalValueHeld)
        steps.push_back(QuitStep::AskIllegalValue);
    if (state.sampleChanged)
        steps.push_back(QuitStep::AskSampleEditor);
    if (state.newClassOpen)
        steps.push_back(QuitStep::AskClassEditor);
    if (state.projectChanged)
        steps.push_back(QuitStep::AskProject);
    steps.push_back(QuitStep::SavePreferences);
    steps.push_back(QuitStep::Exit);
    return steps;
}
}
