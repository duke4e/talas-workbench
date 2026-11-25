#include "app/workbench/WorkbenchComponentInternal.h"
#include "talas/project/SmalltalkSubset.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/GraphBranchLayout.h"
#include "talas/app/TalasKeyEquivalents.h"
#include "talas/app/WorkbenchUiPolicy.h"

namespace talas::app::workbench
{
juce::ApplicationCommandTarget* WorkbenchComponent::getNextCommandTarget()
{
    return nullptr;
}

void WorkbenchComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    for (const auto command : {
             commandNewProject,
             commandOpenProject,
             commandSaveProject,
             commandSaveProjectAs,
             commandExportAudio,
             commandExportPlugin,
             commandPackageProject,
             commandAudioDeviceSettings,
             commandUndo,
             commandRedo,
             commandDeleteSelectedNode,
             commandCutSelectedNodes,
             commandCopySelectedNodes,
             commandPasteGraphNodes,
             commandDuplicateSelectedNodes,
             commandSelectAllGraphNodes,
             commandRenameGraphNode,
             commandCleanUpGraph,
             commandShowExpressionsLibrary,
             commandShowSampleEditor,
             commandShowGlobalMap,
             commandImportMidiToTimeline,
             commandShowClassEditor,
             commandShowAlternateTunings,
             commandShowStructureAsText,
             commandEvaluateSelection,
             commandShowEnvironment,
             commandResetEnvironment,
             commandShowDspStatus,
             commandFindText,
             commandFindTextAgain,
             commandReplaceText,
             commandReplaceTextAgain,
             commandCompileAndLoad,
             commandRestart,
             commandRevertSound,
             commandPasteHot,
             commandPasteSpecial,
             commandShowPreferences,
             commandShowFileOrganizer,
             commandShowSoundBrowser,
             commandShowSoundFileWindow,
             commandOpenCustomPrototypes,
             commandShowSystemPrototypes,
             commandNewTool,
             commandOpenTool,
             commandUseTool,
             commandView,
             commandNewFile,
             commandFileArchivist,
             commandShowMidiMessages,
             commandDescribeSound,
             commandGetInfo,
             commandRecordToDisk,
             commandCompileToDisk,
             commandShowVirtualControlSurface,
             commandNewClassFromExample,
             commandEditClass,
             commandRetrieveExampleFromClass,
             commandSetDefaultSound,
             commandSetDefaultCollection,
             commandChooseGlobalMap,
             commandInfoOscilloscope,
             commandInfoSpectrumAnalyzer,
             commandExpandSound,
             commandMidiNotesOff,
             commandCloseWindow,
             commandPlayMarked,
             commandPlayAll,
             commandOpenAny,
             commandPlayFile,
             commandChooseWindow,
             commandNextWindow,
             commandPreviousWindow,
             commandLargeWindow,
             commandShowFullWaveform,
             commandFocusPrototypeSearch,
             commandCaptureDiskCacheLoop,
             commandPlay,
             commandPause,
             commandStop,
             commandTogglePlayback,
             commandToggleLibrary,
             commandToggleRightWorkspace,
             commandFocusGraph,
             commandFocusTimeline,
             commandZoomGraphToFit,
             commandZoomIn,
             commandZoomOut,
             commandWorkspacePatch,
             commandWorkspaceAutomation,
             commandWorkspaceAnalysis,
             commandWorkspacePerformance,
             commandWorkspaceSamples,
             commandStructureSplitAdaptive,
             commandStructureSplitLeftRight,
             commandStructureSplitTopBottom,
             commandSwapStructurePanes,
             commandToggleSecondaryStructurePane,
             commandGraphViewCompact,
             commandGraphViewPorts,
             commandGraphViewPortsAndWires,
             commandShowWorkbenchGuide,
             commandToggleContextHelp,
             commandShowFakeKeyboard,
             commandShowTapeRecorder,
             commandShowLiveGridEditor,
             commandShowSpectralAnalysis,
             commandShowReAnalysis,
             commandShowSynchronizingSpectra,
             commandConfigureMidi,
             commandInitializeDsp,
             commandShowCdpProcess,
             commandShowGaAnalysis,
         })
    {
        commands.add(command);
    }
}

void WorkbenchComponent::getCommandInfo(
    juce::CommandID commandId,
    juce::ApplicationCommandInfo& info)
{
    const auto commandModifiers = juce::ModifierKeys::commandModifier;
    const auto shiftCommandModifiers =
        commandModifiers | juce::ModifierKeys::shiftModifier;
    const auto optionCommandModifiers =
        commandModifiers | juce::ModifierKeys::altModifier;
    const auto fileOperation = fileOperationCoordinator_.snapshot();
    const auto fileOperationCancellable =
        fileOperation.phase == talas::app::WorkbenchOperationPhase::Running;
    const auto fileOperationCancelling =
        fileOperation.phase == talas::app::WorkbenchOperationPhase::Cancelling;
    const auto setInfo = [&info](
                             const juce::String& name,
                             const juce::String& description,
                             const juce::String& category) {
        info.setInfo(name, description, category, 0);
    };

    switch (commandId)
    {
    case commandNewProject:
        setInfo("New", "Create a new project", "File");
        info.setActive(!destructivePromptActive_ && !fileChooserActive_);
        break;
    case commandOpenProject:
        setInfo("Open...", "Open a file of the type chosen: a Sound file, a Timeline, a Tool to edit or use, or any", "File");
        info.addDefaultKeypress(talas::app::talasKeyEquivalent(talas::app::TalasKeyCommand::Open).key, commandModifiers);
        info.setActive(!destructivePromptActive_ && !fileChooserActive_);
        break;
    case commandSaveProject:
        setInfo("Save", "Save the current project", "File");
        info.addDefaultKeypress(talas::app::talasKeyEquivalent(talas::app::TalasKeyCommand::Save).key, commandModifiers);
        info.setActive(
            !packageRunning_ && !exportRunning_ && !fileChooserActive_);
        break;
    case commandSaveProjectAs:
        setInfo("Save As...", activeGraphContext_.has_value() ? "Save a copy of the Sound in its editor, the editor left open"
                                                              : "Save the current project to a new location", "File");
        info.addDefaultKeypress('s', shiftCommandModifiers);
        info.setActive(
            !packageRunning_ && !exportRunning_ && !fileChooserActive_
            && !diskCacheCaptureRunning_
            && !sampleMapImportOwnsProjectBoundary());
        break;
    case commandExportAudio:
        setInfo(
            exportRunning_
                ? fileOperationCancellable
                    ? "Cancel Audio Export"
                    : fileOperationCancelling
                        ? "Cancelling Audio Export"
                        : "Finishing Audio Export"
                : "Export Audio...",
            "Render the current project to an audio file",
            "File");
        info.setActive(
            !packageRunning_ && !fileChooserActive_
            && (!fileOperation.active() || exportRunning_)
            && (!exportRunning_ || fileOperationCancellable));
        break;
    case commandExportPlugin:
        setInfo("Export Plugin...", "Export the project as a VST3 or AU instrument or effect", "File");
        info.setActive(!packageRunning_ && !exportRunning_ && !fileChooserActive_);
        break;
    case commandPackageProject:
        setInfo(
            packageRunning_
                ? fileOperationCancellable
                    ? "Cancel Packaging"
                    : fileOperationCancelling
                        ? "Cancelling Packaging"
                        : "Finishing Packaging"
                : "Package Project...",
            "Create a redistributable project package",
            "File");
        info.setActive(
            !exportRunning_ && !fileChooserActive_
            && (!fileOperation.active() || packageRunning_)
            && (!packageRunning_ || fileOperationCancellable));
        break;
    case commandAudioDeviceSettings:
        setInfo("Audio Device Settings...", "Choose audio and MIDI hardware", menuFor(talas::app::TalasMenuOperation::AudioSettings));
        info.setActive(hardwareEnabled_);
        break;
    case commandCaptureDiskCacheLoop:
    {
        const auto presentation = diskCacheCapturePresentation(
            activeGraphContext_, selectedNodeId_);
        setInfo(
            presentation.label.isNotEmpty()
                ? presentation.label
                : juce::String{"Capture Disk Cache Loop"},
            presentation.detail.isNotEmpty()
                ? presentation.detail
                : juce::String{"Capture the selected DiskCache Timeline loop"},
            "Edit");
        info.setActive(presentation.visible && presentation.enabled);
        break;
    }
    case commandUndo: // The window in front's.
        setInfo("Undo", "Undo the last edit made in the window in front", "Edit");
        info.addDefaultKeypress('z', commandModifiers);
        info.setActive(projectBridge_.editor().canUndoIn(frontHistoryWindow()));
        break;
    case commandRedo:
        setInfo("Redo", "Redo the last edit undone in the window in front", "Edit");
        info.addDefaultKeypress('z', shiftCommandModifiers);
        info.setActive(projectBridge_.editor().canRedoIn(frontHistoryWindow()));
        break;
    case commandDeleteSelectedNode:
        setInfo("Delete Selected Nodes", "Remove the selected graph nodes from the active Structure scope", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && !selectedNodeId_.empty());
        break;
    case commandCutSelectedNodes:
        setInfo("Cut", "Copy the selected nodes and their connections to the clipboard, then remove them", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && !selectedNodeIds_.empty());
        break;
    case commandCopySelectedNodes:
        setInfo("Copy", "Copy the selected nodes and the connections among them to the clipboard", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && !selectedNodeIds_.empty());
        break;
    case commandShowFakeKeyboard:
        setInfo("Fake Keyboard", "Show keys that play MIDI notes into the live Sound", menuFor(talas::app::TalasMenuOperation::OnScreenKeyboard));
        break;
    case commandShowTapeRecorder:
        setInfo("Tape Recorder", "Record the audio inputs to a sample file", menuFor(talas::app::TalasMenuOperation::TapeRecorder));
        break;
    case commandShowExpressionsLibrary:
        setInfo("Expressions Library", "Find common expressions to drag into parameter fields", "Tools");
        break;
    case commandShowSampleEditor:
        setInfo("Sample Editor", "Create, draw, edit and generate sample files", "Tools");
        break;
    case commandShowGlobalMap:
        setInfo("Global Map", "Edit the associations of Event Values with MIDI controllers", "Tools");
        break;
    case commandImportMidiToTimeline:
        setInfo("Import MIDI File to Timeline...", "Play a MIDI file from a new Timeline track", "Tools");
        break;
    case commandShowClassEditor:
        setInfo("Class Editor", "Edit a Sound's parameter types, ranges, fields, comments and description", "Tools");
        break;
    case commandShowAlternateTunings:
        setInfo("Design Alternate Tunings", "Design tunings of the 128 MIDI keys and try them from a MIDI keyboard",
            menuFor(talas::app::TalasMenuOperation::DesignAlternateTunings));
        break;
    case commandShowStructureAsText:
        setInfo("Structure as Text", "Show the Sound's structure as text, each Sound with its inputs",
            menuFor(talas::app::TalasMenuOperation::StructureAsText));
        break;
    case commandShowEnvironment:
        setInfo("Environment", "Show the values given to the variables of the Sounds played", menuFor(talas::app::TalasMenuOperation::Environment));
        info.setActive(talas::app::talasMenuOperationApplies(talas::app::TalasMenuOperation::Environment, talasMenuContext()));
        break;
    case commandShowPreferences:
        setInfo("Preferences...", "Choose how Talas starts, plays and opens files; kept in your own preferences file",
            menuFor(talas::app::TalasMenuOperation::Preferences));
        break;
    case commandShowFullWaveform:
        setInfo("Full Waveform...", "Record the selected Sound for a time and draw it with its minimum and maximum",
            menuFor(talas::app::TalasMenuOperation::FullWaveform));
        break;
    case commandShowFileOrganizer:
        setInfo("File Organizer", "List the samples, spectra, analyses and MIDI files in your folders", "File");
        break;
    case commandOpenAny:
    case commandPlayFile:
    case commandChooseWindow:
    case commandNextWindow:
    case commandPreviousWindow:
    case commandLargeWindow:
    case commandZoomIn:
    case commandZoomOut:
    case commandShowSoundFileWindow:
    case commandOpenCustomPrototypes:
    case commandShowSystemPrototypes:
    case commandNewTool:
    case commandOpenTool:
    case commandUseTool:
    case commandView:
    case commandNewFile:
    case commandFileArchivist:
    case commandShowMidiMessages:
    case commandConfigureMidi:
    case commandInitializeDsp:
    case commandDescribeSound:
    case commandGetInfo:
    case commandRecordToDisk:
    case commandCompileToDisk:
    case commandShowVirtualControlSurface:
    case commandNewClassFromExample:
    case commandEditClass:
    case commandRetrieveExampleFromClass:
    case commandSetDefaultSound:
    case commandSetDefaultCollection:
    case commandChooseGlobalMap:
    case commandInfoOscilloscope:
    case commandInfoSpectrumAnalyzer:
    case commandExpandSound:
    case commandMidiNotesOff:
    case commandCloseWindow:
    case commandFocusPrototypeSearch:
    case commandFindText:
    case commandFindTextAgain:
    case commandReplaceText:
        windowMenuCommandInfo(info.commandID, info);
        break;
    case commandPlayMarked:
    case commandPlayAll:
        // Shift+Space compiles and plays the marked
        // Sounds alone, Ctrl+Space all of them again.
        setInfo(info.commandID == commandPlayMarked ? "Play Marked Sounds" : "Play All Sounds",
            info.commandID == commandPlayMarked ? "Compile and play only the Timeline's marked Sounds"
                                                : "Compile and play every Sound of the Timeline",
            menuFor(info.commandID == commandPlayMarked ? talas::app::TalasMenuOperation::PlayMarked
                                                        : talas::app::TalasMenuOperation::PlayAll));
        info.defaultKeypresses.add(transportKeyPress(info.commandID == commandPlayMarked
                ? talas::app::TalasTransportCommand::PlayMarked
                : talas::app::TalasTransportCommand::RewindAndPlayAll));
        info.setActive(hardwareInitialised_ && !usesExternalMidiClock());
        break;
    case commandShowSoundBrowser:
        setInfo("Sound Browser", "Find, play and try out the Sounds, samples and files in your folders", "File");
        info.addDefaultKeypress('b', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
        break;
    case commandPasteSpecial:
        setInfo("Paste Special", "Put the copied Sound itself, not a copy, in the selected Sound's place", "Edit");
        break;
    case commandPasteHot:
        setInfo("Paste Hot...", "Paste a commonly used expression or an Event Value name from the global map", "Edit");
        info.addDefaultKeypress('h', juce::ModifierKeys::ctrlModifier);
        break;
    case commandRevertSound:
        setInfo("Revert", "Drop the changes to the selected Sound's parameters since it was opened", menuFor(talas::app::TalasMenuOperation::Revert));
        info.setActive(talas::app::talasMenuOperationApplies(talas::app::TalasMenuOperation::Revert, talasMenuContext()));
        break;
    case commandCompileAndLoad:
        setInfo("Compile & Load", "Compile and load the project without starting it", menuFor(talas::app::TalasMenuOperation::CompileLoad));
        break;
    case commandRestart:
        setInfo("Restart", "Start the loaded project again from the beginning", menuFor(talas::app::TalasMenuOperation::Restart));
        info.addDefaultKeypress('r', juce::ModifierKeys::commandModifier);
        break;
    case commandReplaceTextAgain:
        setInfo("Replace Again", "Replace the next occurrence of the last text replaced", "Edit");
        break;
    case commandShowDspStatus:
        setInfo("DSP Status", "Show the time since the Sound started, MIDI time code, levels and peaks", menuFor(talas::app::TalasMenuOperation::Status));
        break;
    case commandResetEnvironment:
        setInfo("Reset Environment", "Forget the variables' values, so the next play asks for them again",
            menuFor(talas::app::TalasMenuOperation::ResetEnvironment));
        break;
    case commandEvaluateSelection:
        setInfo("Evaluate", "Evaluate the selected text as Smalltalk and put the result after it", menuFor(talas::app::TalasMenuOperation::Evaluate));
        info.addDefaultKeypress('y', juce::ModifierKeys::commandModifier);
        break;
    case commandShowSpectralAnalysis:
        setInfo("Spectral Analysis...", "Analyze a sample into a spectrum file and an example Sound",
            menuFor(talas::app::TalasMenuOperation::SpectralAnalysis));
        break;
    case commandShowGaAnalysis:
        setInfo("GA Analysis...", "Analyze a sample into complex waveforms and envelopes for a GAOscillators",
            menuFor(talas::app::TalasMenuOperation::GaAnalysis));
        break;
    case commandShowCdpProcess:
        setInfo("CDP Process to Sample...", "Render samples through a CDP8 spectral process that changes their length or order", "Tools");
        break;
    case commandShowReAnalysis:
        setInfo("RE Analysis...", "Analyze a sample into RE and EX files for an REResonator", menuFor(talas::app::TalasMenuOperation::ReAnalysis));
        break;
    case commandShowSynchronizingSpectra:
        setInfo("Synchronizing Spectra...", "Mark corresponding points of two spectra to mix or morph them",
            menuFor(talas::app::TalasMenuOperation::SynchronizingSpectra));
        break;
    case commandShowLiveGridEditor:
        setInfo("Live Grid Editor", "Put Sounds into Live Grid boxes, set program channels and step Live Tools", "Tools");
        break;
    case commandPasteGraphNodes:
        setInfo("Paste", "Paste copied nodes into the active Structure scope", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && graphForContext(activeGraphContext_) != nullptr);
        break;
    case commandDuplicateSelectedNodes:
        setInfo("Duplicate Selected Nodes", "Duplicate the selected subgraph in the active Structure scope", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && !selectedNodeIds_.empty());
        break;
    case commandSelectAllGraphNodes:
        setInfo("Select All Graph Nodes", "Select every node in the active Structure scope", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && graphForContext(activeGraphContext_) != nullptr);
        break;
    case commandRenameGraphNode:
        setInfo("Rename Sound...", "Give the selected Sound a new name", "Edit");
        info.addDefaultKeypress(juce::KeyPress::F2Key, juce::ModifierKeys::noModifiers);
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && selectedNodeIds_.size() == 1);
        break;
    case commandCleanUpGraph:
        setInfo("Clean Up", "Lay the signal flow, or the surface in front, out in columns", "Edit");
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples
            && graphForContext(activeGraphContext_) != nullptr);
        break;
    case commandPlay:
        setInfo(
            latestPreparedGraphStatus_.activationRejected ? "Retry Score Change" : "Play",
            latestPreparedGraphStatus_.activationRejected
                ? "Apply the Score change again without changing transport playback"
                : usesExternalMidiClock()
                ? "Transport is controlled by the selected MIDI Clock input"
                : "Play the current project",
            menuFor(talas::app::TalasMenuOperation::CompileLoadStart));
        info.defaultKeypresses.add(transportKeyPress(talas::app::TalasTransportCommand::Play));
        info.setActive(
            hardwareInitialised_
            && (latestPreparedGraphStatus_.activationRejected
                || (!usesExternalMidiClock() && !transportPlaying_)));
        break;
    case commandPause:
        setInfo(
            "Pause",
            usesExternalMidiClock()
                ? "Transport is controlled by the selected MIDI Clock input"
                : "Pause at the current Timeline position",
            menuFor(talas::app::TalasMenuOperation::Pause));
        info.setActive(
            !usesExternalMidiClock()
            && hardwareInitialised_ && transportPlaying_);
        break;
    case commandStop:
        setInfo(
            "Stop",
            usesExternalMidiClock()
                ? "Transport is controlled by the selected MIDI Clock input"
                : "Stop and return to the start",
            menuFor(talas::app::TalasMenuOperation::Stop));
        // Ctrl+K kills the Sound.
        info.defaultKeypresses.add(transportKeyPress(talas::app::TalasTransportCommand::Stop));
        info.setActive(
            !usesExternalMidiClock() && hardwareInitialised_);
        break;
    case commandTogglePlayback:
        setInfo(
            "Play/Pause",
            usesExternalMidiClock()
                ? "Transport is controlled by the selected MIDI Clock input"
                : "Toggle Timeline playback",
            menuFor(talas::app::TalasMenuOperation::Pause));
        info.defaultKeypresses.add(transportKeyPress(talas::app::TalasTransportCommand::PlayPause));
        info.setActive(
            !usesExternalMidiClock() && hardwareInitialised_);
        break;
    case commandToggleLibrary:
        setInfo("Show Library", "Show or hide the Prototype and Sound Definition library", "View");
        info.setTicked(
            talas::app::workbenchWorkspaceVisibility(workspaceMode_).library
            && (talas::app::workbenchLayoutPolicyForSize(
                    getWidth(),
                    getHeight())
                        .useSummonableSideWorkspaces
                    ? compactDock_ == CompactDock::Library
                    : libraryVisible_));
        break;
    case commandToggleRightWorkspace:
        setInfo("Show Right Workspace", "Show or hide Inspector, Timeline, and Analysis", "View");
        info.setTicked(
            talas::app::workbenchLayoutPolicyForSize(
                getWidth(),
                getHeight())
                    .useSummonableSideWorkspaces
                ? compactDock_ == CompactDock::RightWorkspace
                : rightWorkspaceVisible_);
        break;
    case commandFocusGraph:
        setInfo("Focus Graph", "Move keyboard focus to the signal graph", "View");
        info.addDefaultKeypress('1', commandModifiers);
        break;
    case commandFocusTimeline:
        setInfo("Focus Timeline", "Move keyboard focus to Timeline events", "View");
        info.addDefaultKeypress('2', commandModifiers);
        break;
    case commandZoomGraphToFit:
        setInfo(
            "Zoom Graph to Fit",
            "Frame the complete Sound graph in the available canvas",
            "View");
        info.addDefaultKeypress('0', commandModifiers);
        break;
    case commandWorkspacePatch:
        setInfo(
            "Patch Workspace",
            "Focus prototype discovery, graph construction, inspection, and live controls",
            "View");
        info.addDefaultKeypress('1', shiftCommandModifiers);
        info.setTicked(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch);
        break;
    case commandWorkspaceAutomation:
        setInfo(
            "Automation Workspace",
            "Focus Timeline editing beside the graph and live controls",
            "View");
        info.addDefaultKeypress('2', shiftCommandModifiers);
        info.setTicked(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Automation);
        break;
    case commandWorkspaceAnalysis:
        setInfo(
            "Analysis Workspace",
            "Focus measurement tools beside an expanded graph",
            "View");
        info.addDefaultKeypress('3', shiftCommandModifiers);
        info.setTicked(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis);
        break;
    case commandWorkspacePerformance:
        setInfo(
            "Performance Workspace",
            "Operate stable exposed controls for the selected reusable Sound instance",
            "View");
        info.addDefaultKeypress('4', shiftCommandModifiers);
        info.setTicked(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
        break;
    case commandWorkspaceSamples:
        setInfo(
            "Samples Workspace",
            "Build note and velocity maps, edit exact waveform loops, and audition sample assets",
            "View");
        info.addDefaultKeypress('5', shiftCommandModifiers);
        info.setTicked(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples);
        break;
    case commandStructureSplitAdaptive:
        setInfo(
            "Adaptive Structure Split",
            "Choose left/right or top/bottom Structure panes from the available size",
            "View");
        info.addDefaultKeypress('a', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.structureSplitOrientation
            == talas::project::WorkspaceStructureSplitOrientation::Adaptive);
        info.setActive(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
            || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
        break;
    case commandStructureSplitLeftRight:
        setInfo(
            "Left/Right Structure Split",
            "Keep the two Structure panes side by side",
            "View");
        info.addDefaultKeypress('l', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.structureSplitOrientation
            == talas::project::WorkspaceStructureSplitOrientation::LeftRight);
        info.setActive(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
            || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
        break;
    case commandStructureSplitTopBottom:
        setInfo(
            "Top/Bottom Structure Split",
            "Keep the two Structure panes stacked",
            "View");
        info.addDefaultKeypress('t', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.structureSplitOrientation
            == talas::project::WorkspaceStructureSplitOrientation::TopBottom);
        info.setActive(
            workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
            || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance);
        break;
    case commandSwapStructurePanes:
        setInfo(
            "Swap Structure Panes",
            "Move the primary and secondary Structure panes to opposite sides without changing edit scope",
            "View");
        info.addDefaultKeypress('s', optionCommandModifiers);
        info.setActive(
            secondaryGraphContext_.has_value()
            && workspaceSession_.secondaryPaneVisible
            && (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
                || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance));
        break;
    case commandToggleSecondaryStructurePane:
        setInfo(
            workspaceSession_.secondaryPaneVisible
                ? "Close Secondary Structure Pane"
                : "Restore Secondary Structure Pane",
            workspaceSession_.secondaryPaneVisible
                ? "Hide the secondary pane while retaining its context and transform"
                : "Restore the retained secondary Structure context",
            "View");
        info.addDefaultKeypress('p', optionCommandModifiers);
        info.setTicked(
            secondaryGraphContext_.has_value()
            && workspaceSession_.secondaryPaneVisible);
        info.setActive(
            secondaryGraphContext_.has_value()
            && (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
                || workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Performance));
        break;
    case commandGraphViewCompact:
        setInfo(
            "Compact Graph",
            "Show node identity cards without ports or wires",
            "View");
        info.addDefaultKeypress('1', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.graphViewMode
            == talas::project::WorkspaceGraphViewMode::Compact);
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples);
        break;
    case commandGraphViewPorts:
        setInfo(
            "Graph with Ports",
            "Show typed ports without connection wires",
            "View");
        info.addDefaultKeypress('2', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.graphViewMode
            == talas::project::WorkspaceGraphViewMode::Ports);
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples);
        break;
    case commandGraphViewPortsAndWires:
        setInfo(
            "Graph with Ports and Wires",
            "Show typed ports and semantic connections",
            "View");
        info.addDefaultKeypress('3', optionCommandModifiers);
        info.setTicked(
            workspaceSession_.graphViewMode
            == talas::project::WorkspaceGraphViewMode::PortsAndWires);
        info.setActive(
            workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Samples);
        break;
    case commandShowWorkbenchGuide:
        setInfo(
            "Workbench Guide",
            "Show workspace, graph, signal, and prototype guidance",
            "Help");
        info.addDefaultKeypress('/', shiftCommandModifiers);
        break;
    case commandToggleContextHelp:
        setInfo(
            "Context Help",
            "Show or hide compact help for the currently focused control",
            "View");
        info.setTicked(
            workspaceSession_.accessibility.contextHelpVisible);
        break;
    default:
        break;
    }
    (void)activeFieldCommandInfo(info);
}

bool WorkbenchComponent::perform(const juce::ApplicationCommandTarget::InvocationInfo& info)
{
    if (editActiveField(info.commandID))
        return true;
    // An operation of the Action or Info menu first tests the
    // value typed into the field, and is refused while it is illegal.
    if (inspectorPanel_ != nullptr
        && talas::app::typedFieldValueTested(fieldTestMomentOfCommand(info.commandID))
        && !inspectorPanel_->testTypedValue())
    {
        juce::LookAndFeel::getDefaultLookAndFeel().playAlertSound();
        setStatus("Fix the illegal value, or Revert, before choosing that operation");
        return true;
    }
    switch (info.commandID)
    {
    case commandNewProject:
        requestNewProject();
        return true;
    case commandOpenProject:
        openFileOfChosenType();
        return true;
    case commandSaveProject:
        saveProject();
        // The Sound in its editor is saved as it is now.
        talas::app::soundEditorsSaved(soundEditors_, projectBridge_.editor().document());
        return true;
    case commandSaveProjectAs:
        // With a Sound's editor in front, a copy of
        // the Sound, the editor left open on it.
        // With the spectrum editor in front, the spectrum.
        if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Analysis && partialTrackEditorPanel_->promptSaveAs())
            return true;
        if (activeGraphContext_.has_value())
            saveSoundCopy(*activeGraphContext_);
        else
            offerObsoleteMaterialRemoval([this] { saveProjectAs(); });
        return true;
    case commandExportAudio:
        exportRunning_ ? cancelAudioExport() : showExportAudioDialog();
        return true;
    case commandExportPlugin:
        showExportPluginDialog();
        return true;
    case commandPackageProject:
        packageRunning_ ? cancelProjectPackage() : choosePackageDestination();
        return true;
    case commandAudioDeviceSettings:
        showAudioSettings();
        return true;
    case commandCaptureDiskCacheLoop:
        performDiskCacheCaptureAction(
            activeGraphContext_, selectedNodeId_);
        return true;
    case commandUndo:
        undoProject(frontHistoryWindow());
        return true;
    case commandRedo:
        redoProject(frontHistoryWindow());
        return true;
    case commandDeleteSelectedNode:
        deleteSelectedNode();
        return true;
    case commandCutSelectedNodes:
        cutSelectedNodes();
        return true;
    case commandCopySelectedNodes:
        (void)copySelectedNodes();
        return true;
    case commandPasteGraphNodes:
        pasteGraphNodes();
        return true;
    case commandShowTapeRecorder:
        showTapeRecorder();
        return true;
    case commandShowFakeKeyboard:
        showFakeKeyboard();
        return true;
    case commandShowExpressionsLibrary:
        showExpressionsLibrary();
        return true;
    case commandShowSampleEditor:
        showSampleEditor();
        return true;
    case commandShowGlobalMap:
        showGlobalMap();
        return true;
    case commandImportMidiToTimeline:
        chooseMidiFileForTimeline();
        return true;
    case commandShowClassEditor:
        showClassEditor();
        return true;
    case commandShowAlternateTunings:
        showAlternateTunings();
        return true;
    case commandShowStructureAsText:
        showStructureAsText();
        return true;
    case commandShowEnvironment:
        showVariableEnvironment();
        return true;
    case commandResetEnvironment:
        resetVariableEnvironment();
        return true;
    case commandShowDspStatus:
        showDspStatus();
        return true;
    case commandRevertSound:
        revertSelectedSound();
        return true;
    case commandPasteHot:
        pasteHotIntoFocusedField();
        return true;
    case commandPasteSpecial:
        pasteSpecialGraphNode();
        return true;
    case commandShowPreferences:
        showPreferencesDialog();
        return true;
    case commandShowFileOrganizer:
        showFileOrganizer();
        return true;
    case commandShowSoundBrowser:
        showSoundBrowser();
        return true;
    case commandPlayMarked:
    case commandPlayAll:
        playTimelineMarked(info.commandID == commandPlayMarked);
        return true;
    case commandOpenAny:
    case commandPlayFile:
    case commandChooseWindow:
    case commandNextWindow:
    case commandPreviousWindow:
    case commandLargeWindow:
    case commandZoomIn:
    case commandZoomOut:
    case commandShowSoundFileWindow:
    case commandOpenCustomPrototypes:
    case commandShowSystemPrototypes:
    case commandNewTool:
    case commandOpenTool:
    case commandUseTool:
    case commandView:
    case commandNewFile:
    case commandFileArchivist:
    case commandShowMidiMessages:
    case commandConfigureMidi:
    case commandInitializeDsp:
    case commandDescribeSound:
    case commandGetInfo:
    case commandRecordToDisk:
    case commandCompileToDisk:
    case commandShowVirtualControlSurface:
    case commandNewClassFromExample:
    case commandEditClass:
    case commandRetrieveExampleFromClass:
    case commandSetDefaultSound:
    case commandSetDefaultCollection:
    case commandChooseGlobalMap:
    case commandInfoOscilloscope:
    case commandInfoSpectrumAnalyzer:
    case commandExpandSound:
    case commandMidiNotesOff:
    case commandCloseWindow:
        performWindowMenuCommand(info.commandID);
        return true;
    case commandShowFullWaveform:
        showFullWaveform();
        return true;
    case commandCompileAndLoad:
        (void) compileAndLoadCurrentProject();
        return true;
    case commandRestart:
        pressTransportKey(talas::app::TalasTransportPress::ControlR);
        return true;
    case commandFindText:
    case commandFindTextAgain:
        findInFocusedField(info.commandID == commandFindTextAgain);
        return true;
    case commandReplaceText:
    case commandReplaceTextAgain:
        replaceInFocusedField(info.commandID == commandReplaceTextAgain);
        return true;
    case commandEvaluateSelection:
        if (auto* editor = dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()))
            evaluateSelectionIn(*editor);
        else
            setStatus("Select an expression in a text field to evaluate it");
        return true;
    case commandShowLiveGridEditor:
        showLiveGridEditor();
        return true;
    case commandShowSpectralAnalysis:
        showAnalysisTool(AnalysisToolPanel::Kind::Spectral);
        return true;
    case commandShowReAnalysis:
        showAnalysisTool(AnalysisToolPanel::Kind::Re);
        return true;
    case commandShowSynchronizingSpectra:
        showSynchronizingSpectra();
        return true;
    case commandShowCdpProcess:
        showCdpProcessTool();
        return true;
    case commandShowGaAnalysis:
        showAnalysisTool(AnalysisToolPanel::Kind::Ga);
        return true;
    case commandDuplicateSelectedNodes:
        duplicateSelectedNodes();
        return true;
    case commandSelectAllGraphNodes:
        selectAllGraphNodes();
        return true;
    case commandFocusPrototypeSearch:
        focusPrototypeSearch();
        return true;
    case commandRenameGraphNode:
        if (selectedNodeIds_.size() == 1)
            promptRenameGraphNode(activeGraphContext_, selectedNodeIds_.front());
        return true;
    case commandCleanUpGraph:
        cleanUpFrontWindow();
        return true;
    case commandPlay:
        playCurrentProject();
        return true;
    case commandPause:
        pauseCurrentProject();
        return true;
    case commandStop:
        // With Update display during disk operations off,
        // Ctrl+K leaves a Sound on the disk to Shift+click.
        if (!talas::app::interfaceRunsDuringDiskOperation(transportPlaying_ && talas::app::loadedSoundInputs(projectBridge_.editor().document()).usesDisk,
                preferences_.updateDisplayDuringDiskOperations))
        {
            setStatus("The Sound uses the disk: Shift+click stops it while the display does not update");
            return true;
        }
        stopCurrentProject();
        return true;
    case commandTogglePlayback:
        pressTransportKey(talas::app::TalasTransportPress::Space);
        return true;
    case commandToggleLibrary:
    {
        const auto policy = talas::app::workbenchLayoutPolicyForSize(
            getWidth(),
            getHeight());
        if (!talas::app::workbenchWorkspaceVisibility(workspaceMode_).library)
        {
            activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
            libraryVisible_ = true;
            compactDock_ = policy.useSummonableSideWorkspaces
                ? CompactDock::Library
                : CompactDock::None;
            refreshWorkspaceButtons();
            resized();
            setStatus("Patch workspace opened with Library");
            commandManager_.commandStatusChanged();
            return true;
        }
        if (policy.useSummonableSideWorkspaces)
        {
            compactDock_ = compactDock_ == CompactDock::Library
                ? CompactDock::None
                : CompactDock::Library;
        }
        else
        {
            libraryVisible_ = !libraryVisible_;
        }
        resized();
        setStatus(
            (policy.useSummonableSideWorkspaces
                 ? compactDock_ == CompactDock::Library
                 : libraryVisible_)
                ? "Library shown"
                : "Library hidden");
        commandManager_.commandStatusChanged();
        return true;
    }
    case commandToggleRightWorkspace:
    {
        const auto policy = talas::app::workbenchLayoutPolicyForSize(
            getWidth(),
            getHeight());
        if (policy.useSummonableSideWorkspaces)
        {
            compactDock_ = compactDock_ == CompactDock::RightWorkspace
                ? CompactDock::None
                : CompactDock::RightWorkspace;
        }
        else
        {
            rightWorkspaceVisible_ = !rightWorkspaceVisible_;
        }
        resized();
        setStatus(
            (policy.useSummonableSideWorkspaces
                 ? compactDock_ == CompactDock::RightWorkspace
                 : rightWorkspaceVisible_)
                ? "Right workspace shown"
                : "Right workspace hidden");
        commandManager_.commandStatusChanged();
        return true;
    }
    case commandFocusGraph:
        if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples)
        {
            activateWorkspaceMode(
                talas::app::WorkbenchWorkspaceMode::Patch);
        }
        if (activeGraphContext_ == secondaryGraphContext_
            && secondaryGraphContext_.has_value())
            secondaryGraphCanvas_->focusGraph();
        else
            graphCanvas_->focusGraph();
        return true;
    case commandFocusTimeline:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Automation);
        timelinePanel_->focusTimeline();
        return true;
    case commandZoomGraphToFit:
        if (activeGraphContext_ == secondaryGraphContext_
            && secondaryGraphContext_.has_value())
            secondaryGraphCanvas_->zoomToFit();
        else
            graphCanvas_->zoomToFit();
        return true;
    case commandWorkspacePatch:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Patch);
        return true;
    case commandWorkspaceAutomation:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Automation);
        return true;
    case commandWorkspaceAnalysis:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Analysis);
        return true;
    case commandWorkspacePerformance:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Performance);
        return true;
    case commandWorkspaceSamples:
        activateWorkspaceMode(
            talas::app::WorkbenchWorkspaceMode::Samples);
        return true;
    case commandStructureSplitAdaptive:
        setWorkspaceStructureSplitOrientation(
            talas::project::WorkspaceStructureSplitOrientation::Adaptive);
        setStatus("Adaptive Structure split active");
        return true;
    case commandStructureSplitLeftRight:
        setWorkspaceStructureSplitOrientation(
            talas::project::WorkspaceStructureSplitOrientation::LeftRight);
        setStatus("Left/right Structure split active");
        return true;
    case commandStructureSplitTopBottom:
        setWorkspaceStructureSplitOrientation(
            talas::project::WorkspaceStructureSplitOrientation::TopBottom);
        setStatus("Top/bottom Structure split active");
        return true;
    case commandSwapStructurePanes:
        swapWorkspaceStructurePanes();
        return true;
    case commandToggleSecondaryStructurePane:
        toggleSecondaryWorkspacePane();
        return true;
    case commandGraphViewCompact:
        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::Compact);
        setStatus("Compact graph presentation active");
        return true;
    case commandGraphViewPorts:
        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::Ports);
        setStatus("Graph ports presentation active");
        return true;
    case commandGraphViewPortsAndWires:
        setWorkspaceGraphViewMode(
            talas::project::WorkspaceGraphViewMode::PortsAndWires);
        setStatus("Graph ports-and-wires presentation active");
        return true;
    case commandShowWorkbenchGuide:
        showWorkbenchGuide();
        return true;
    case commandToggleContextHelp:
        workspaceSession_.accessibility.contextHelpVisible =
            !workspaceSession_.accessibility.contextHelpVisible;
        applyAccessibilityPreferences();
        noteWorkspaceSessionChanged();
        commandManager_.commandStatusChanged();
        setStatus(
            workspaceSession_.accessibility.contextHelpVisible
                ? "Context Help shown"
                : "Context Help hidden");
        return true;
    default:
        return false;
    }
}

void WorkbenchComponent::menuItemSelected(
    int menuItemId,
    int)
{
    if (selectSavedTool(menuItemId) || selectDocumentation(menuItemId) || selectFieldReading(menuItemId))
        return;
    const auto index = menuItemId - recentProjectMenuItemBase;
    const auto paths = recentProjects_.entries();
    if (index >= 0 && static_cast<std::size_t>(index) < paths.size())
        requestOpenRecentProject(paths[static_cast<std::size_t>(index)]);
}

void WorkbenchComponent::addPrototype(const talas::core::NodeTypeDescriptor& descriptor)
{
    const auto* graph = graphForContext(activeGraphContext_);
    const auto nodeCount = graph == nullptr ? std::size_t{} : graph->nodes.size();
    addPrototypeAt(activeGraphContext_, descriptor.typeId, {
        72 + static_cast<std::int32_t>(nodeCount % 3) * 210,
        64 + static_cast<std::int32_t>((nodeCount / 3) % 3) * 150,
    });
}

void WorkbenchComponent::handleSoundAction(
    SoundDefinitionsPanel::Action action,
    std::string definitionId)
{
    switch (action)
    {
    case SoundDefinitionsPanel::Action::Create:
        createSoundDefinition();
        break;
    case SoundDefinitionsPanel::Action::Rename:
        renameSoundDefinition(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::Delete:
        deleteSoundDefinition(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::AddInstance:
        addSoundInstance(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::UseInProject:
        useSoundLibraryEntry(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::OpenExample:
        requestOpenSoundLibraryExample(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::OpenHelp:
        openSoundLibraryHelp(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::ExtractSelection:
        extractSelectedNodeToSound();
        break;
    case SoundDefinitionsPanel::Action::InlineSelection:
        inlineSelectedSound();
        break;
    case SoundDefinitionsPanel::Action::OpenDefinition:
        showSoundDefinition(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::ShowRoot:
        showRootGraph();
        break;
    case SoundDefinitionsPanel::Action::OpenParent:
        openParentDefinition();
        break;
    case SoundDefinitionsPanel::Action::Audition:
        startSoundLibraryAudition(std::move(definitionId));
        break;
    case SoundDefinitionsPanel::Action::ControlCurrentProject:
        startSoundLibraryAudition(std::move(definitionId), true);
        break;
    case SoundDefinitionsPanel::Action::ToggleBookmark:
    {
        const auto bookmarked =
            std::ranges::any_of(workspaceSession_.hierarchyBookmarks,
                [&](const auto& bookmark)
                { return bookmark.definitionId == definitionId; });
        if (bookmarked)
        {
            (void)talas::project::removeWorkspaceSessionHierarchyBookmark(
                workspaceSession_,
                definitionId);
            setStatus("Removed local hierarchy bookmark for " +
                      juceString(definitionId));
        }
        else if (!talas::project::captureWorkspaceSessionHierarchyBookmark(
                     workspaceSession_,
                     definitionId))
        {
            setStatus("Could not capture hierarchy bookmark for " +
                      juceString(definitionId));
        }
        else
        {
            setStatus(
                "Bookmarked hierarchy view for " + juceString(definitionId));
        }
        soundDefinitionsPanel_->setBookmarks(
            talas::project::workspaceSessionBookmarkedDefinitionIds(
                workspaceSession_));
        noteWorkspaceSessionChanged();
        break;
    }
    case SoundDefinitionsPanel::Action::RestoreBookmark:
        if (!talas::project::restoreWorkspaceSessionHierarchyBookmark(
                workspaceSession_,
                definitionId))
        {
            setStatus("Hierarchy bookmark is unavailable");
            break;
        }
        applyWorkspaceSessionProjection(true);
        setStatus(
            "Restored hierarchy bookmark for " + juceString(definitionId));
        noteWorkspaceSessionChanged();
        break;
    case SoundDefinitionsPanel::Action::NavigateBreadcrumb:
    {
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
        if (definitionId.empty())
        {
            showRootGraph();
            break;
        }
        const auto target = std::ranges::find(hierarchyPath_, definitionId);
        if (target == hierarchyPath_.end())
        {
            showSoundDefinition(std::move(definitionId));
            break;
        }
        hierarchyPath_.erase(target + 1, hierarchyPath_.end());
        secondaryGraphContext_ = hierarchyPath_.back();
        primaryGraphContext_ =
            hierarchyPath_.size() > 1
                ? std::optional<
                      std::string>{hierarchyPath_[hierarchyPath_.size() - 2]}
                : std::optional<std::string>{};
        workspaceSession_.secondaryPaneVisible = true;
        activeGraphContext_ = secondaryGraphContext_;
        applyHierarchyContexts();
        setStatus(
            "Navigated to hierarchy breadcrumb " + juceString(definitionId));
        break;
    }
    case SoundDefinitionsPanel::Action::ToggleFavorite:
    case SoundDefinitionsPanel::Action::LibraryStateChanged:
    {
        auto nextState = soundDefinitionsPanel_->libraryState();
        if (workspaceSession_.soundLibrary.inputSubstitutions
            != nextState.inputSubstitutions)
        {
            stopSoundLibraryAudition();
        }
        workspaceSession_.soundLibrary = std::move(nextState);
        noteWorkspaceSessionChanged();
        break;
    }
    }
}

void WorkbenchComponent::showSoundDefinition(std::string definitionId)
{
    if (definitionId.empty())
        return;
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    // The Sound's editor, opened on it (the Sound as it
    // opens, for Drop) or brought to the front (refreshSoundEditors).
    primaryGraphContext_.reset();
    secondaryGraphContext_ = definitionId;
    workspaceSession_.secondaryPaneVisible = true;
    activeGraphContext_ = secondaryGraphContext_;
    hierarchyPath_ = {definitionId};
    applyHierarchyContexts();
    setStatus(
        "Editing Project / Root / "
        + juceString(definitionId)
        + " in the opposite Structure pane");
}

void WorkbenchComponent::showRootGraph()
{
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    primaryGraphContext_.reset();
    secondaryGraphContext_.reset();
    activeGraphContext_.reset();
    hierarchyPath_.clear();
    soundDefinitionsPanel_->showRootContext();
    applyHierarchyContexts();
    setStatus("Viewing Project / Root");
}

void WorkbenchComponent::navigateStatusDiagnostic(
    const talas::app::SoundWorkbenchDiagnosticIdentity& identity)
{
    const auto target = talas::app::soundWorkbenchDiagnosticTarget(
        projectBridge_.editor().document(),
        identity);
    if (!target.has_value())
    {
        setStatus(juceString(
            "Prepared diagnostic target is unavailable in the current "
            "authoring "
            "graph · compile the pending project state to refresh it"));
        return;
    }

    activateWorkspaceMode(
        talas::app::WorkbenchWorkspaceMode::Patch);
    if (target->definitionId.has_value())
        showSoundDefinition(*target->definitionId);
    else
        showRootGraph();
    selectNode(target->definitionId, target->nodeId);
    setStatus(
        juceString("Selected diagnostic target · ")
        + juceString(target->nodeId));
}

void WorkbenchComponent::activateStatusDiagnostic(
    const WorkbenchStatusDiagnostic& diagnostic)
{
    const auto& navigation = diagnostic.navigation;
    switch (navigation.kind)
    {
    case talas::app::AnalysisNavigationKind::None:
        navigateStatusDiagnostic(diagnostic.identity);
        return;
    case talas::app::AnalysisNavigationKind::GraphNode:
    case talas::app::AnalysisNavigationKind::Parameter:
        navigateStatusDiagnostic({
            navigation.nodeId,
            navigation.structuralSourcePath,
        });
        if (navigation.kind
                == talas::app::AnalysisNavigationKind::Parameter
            && !navigation.parameterId.empty())
        {
            inspectorPanel_->focusParameter(navigation.parameterId);
        }
        return;
    case talas::app::AnalysisNavigationKind::AssetRecovery:
        rightWorkspaceVisible_ = true;
        compactDock_ = CompactDock::RightWorkspace;
        resized();
        if (assetRecoveryPanel_->focusAsset(navigation.assetId))
        {
            setStatus(
                juceString("Selected asset recovery item · ")
                + juceString(navigation.assetId));
        }
        else
        {
            setStatus(
                juceString("Asset recovery item is no longer available · ")
                + juceString(navigation.assetId));
        }
        return;
    case talas::app::AnalysisNavigationKind::AnalysisTrace:
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
        selectAnalysisTrace(navigation.traceId);
        setStatus(
            juceString("Selected Analysis trace · ")
            + juceString(navigation.traceId));
        return;
    case talas::app::AnalysisNavigationKind::EventMonitor:
    case talas::app::AnalysisNavigationKind::MidiMonitor:
    case talas::app::AnalysisNavigationKind::OscIngressMonitor:
    case talas::app::AnalysisNavigationKind::OscEgressMonitor:
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
        if (analysisPanel_->focusDiagnosticMonitor(
                navigation,
                diagnostic.title + juceString(" · ") + diagnostic.detail))
        {
            setStatus(
                juceString("Selected exact monitor evidence · ")
                + diagnostic.title);
        }
        else
        {
            setStatus("Diagnostic monitor target is unavailable");
        }
        return;
    }
}

void WorkbenchComponent::openChildDefinition(
    std::optional<std::string> parentDefinitionId,
    std::string childDefinitionId)
{
    if (childDefinitionId.empty())
        return;
    if (!parentDefinitionId.has_value())
    {
        hierarchyPath_ = {childDefinitionId};
        primaryGraphContext_.reset();
    }
    else
    {
        const auto parent = std::ranges::find(
            hierarchyPath_,
            *parentDefinitionId);
        if (parent == hierarchyPath_.end())
            hierarchyPath_ = {*parentDefinitionId};
        else
            hierarchyPath_.erase(parent + 1, hierarchyPath_.end());
        hierarchyPath_.push_back(childDefinitionId);
        primaryGraphContext_ = parentDefinitionId;
    }
    secondaryGraphContext_ = childDefinitionId;
    workspaceSession_.secondaryPaneVisible = true;
    activeGraphContext_ = secondaryGraphContext_;
    applyHierarchyContexts();
    setStatus(
        "Opened child "
        + juceString(childDefinitionId)
        + " opposite its parent");
}

void WorkbenchComponent::openParentDefinition()
{
    if (hierarchyPath_.empty())
    {
        showRootGraph();
        return;
    }
    hierarchyPath_.pop_back();
    if (hierarchyPath_.empty())
    {
        showRootGraph();
        return;
    }
    secondaryGraphContext_ = hierarchyPath_.back();
    workspaceSession_.secondaryPaneVisible = true;
    primaryGraphContext_ = hierarchyPath_.size() > 1
        ? std::optional<std::string>{
              hierarchyPath_[hierarchyPath_.size() - 2]}
        : std::optional<std::string>{};
    activeGraphContext_ = secondaryGraphContext_;
    applyHierarchyContexts();
    setStatus("Opened parent Structure context");
}

void WorkbenchComponent::applyHierarchyContexts()
{
    workspaceSession_.structureNavigationPath = hierarchyPath_;
    workspaceSession_.activeSoundDefinitionId = activeGraphContext_;
    talas::project::reconcileWorkspaceSessionState(
        workspaceSession_,
        projectBridge_.editor().document());
    hierarchyPath_ = workspaceSession_.structureNavigationPath;
    primaryGraphContext_ =
        hierarchyPath_.size() > 1
            ? std::optional<std::string>{hierarchyPath_[hierarchyPath_.size() -
                                                        2]}
            : std::optional<std::string>{};
    secondaryGraphContext_ =
        hierarchyPath_.empty()
            ? std::optional<std::string>{}
            : std::optional<std::string>{hierarchyPath_.back()};
    activeGraphContext_ = workspaceSession_.activeSoundDefinitionId;
    graphCanvas_->setSoundDefinitionContext(primaryGraphContext_,
        talas::app::soundWorkbenchSessionSelection(workspaceSession_,
            optionalStringView(primaryGraphContext_)));
    secondaryGraphCanvas_->setSoundDefinitionContext(
        secondaryGraphContext_,
        talas::app::soundWorkbenchSessionSelection(
            workspaceSession_,
            optionalStringView(secondaryGraphContext_)));
    applyWorkspaceGraphView(
        *graphCanvas_,
        primaryGraphContext_);
    applyWorkspaceGraphView(
        *secondaryGraphCanvas_,
        secondaryGraphContext_);
    graphCanvas_->setCompileDiagnostics(
        latestCompileDiagnostics_,
        compileDiagnosticDocument());
    secondaryGraphCanvas_->setCompileDiagnostics(
        latestCompileDiagnostics_,
        compileDiagnosticDocument());
    soundDefinitionsPanel_->setNavigationPath(hierarchyPath_);
    if (secondaryGraphContext_.has_value())
        soundDefinitionsPanel_->selectDefinition(*secondaryGraphContext_);
    const auto restored =
        talas::app::soundWorkbenchSessionSelection(
            workspaceSession_,
            optionalStringView(activeGraphContext_));
    selectNode(activeGraphContext_, restored);
    // The Sound file window marks the Sound being edited.
    if (soundFilePanel_ != nullptr)
        soundFilePanel_->refresh();
    resized();
    refreshSoundEditors();
    commandManager_.commandStatusChanged();
}

[[nodiscard]] juce::String WorkbenchComponent::graphContextPresentation(
    const std::optional<std::string>& definitionId)
{
    return definitionId.has_value()
        ? "SOUND / " + juceString(*definitionId)
        : juce::String{"ROOT"};
}

void WorkbenchComponent::refreshGraphContextPresentation()
{
    graphCanvas_->setContextPresentation(
        graphContextPresentation(primaryGraphContext_),
        activeGraphContext_ == primaryGraphContext_
            && graphCanvas_->isVisible()
            && !graphCanvas_->getBounds().isEmpty());
    secondaryGraphCanvas_->setContextPresentation(
        graphContextPresentation(secondaryGraphContext_),
        activeGraphContext_ == secondaryGraphContext_
            && secondaryGraphCanvas_->isVisible()
            && !secondaryGraphCanvas_->getBounds().isEmpty());
    // The Sound editor's close box at its upper left.
    const auto editorShown = secondaryGraphContext_.has_value() && secondaryGraphCanvas_->isVisible()
        && !secondaryGraphCanvas_->getBounds().isEmpty();
    soundEditorCloseBox_.setVisible(editorShown);
    if (editorShown)
    {
        soundEditorCloseBox_.setBounds(secondaryGraphCanvas_->getX() + 4, secondaryGraphCanvas_->getY() + 4, 22, 22);
        soundEditorCloseBox_.toFront(false);
    }
    layoutSoundEditorTabs();
}

void WorkbenchComponent::reconcileHierarchyAfterHistoryEdit()
{
    const auto firstMissing = std::ranges::find_if(
        hierarchyPath_,
        [&](const auto& definitionId) {
            return graphForContext(
                       std::optional<std::string>{definitionId})
                == nullptr;
        });
    hierarchyPath_.erase(firstMissing, hierarchyPath_.end());

    if (hierarchyPath_.empty())
    {
        primaryGraphContext_.reset();
        secondaryGraphContext_.reset();
        activeGraphContext_.reset();
        soundDefinitionsPanel_->showRootContext();
        applyHierarchyContexts();
        return;
    }

    secondaryGraphContext_ = hierarchyPath_.back();
    primaryGraphContext_ = hierarchyPath_.size() > 1
        ? std::optional<std::string>{
              hierarchyPath_[hierarchyPath_.size() - 2]}
        : std::optional<std::string>{};
    if (activeGraphContext_ != primaryGraphContext_
        && activeGraphContext_ != secondaryGraphContext_)
    {
        activeGraphContext_ = secondaryGraphContext_;
    }
    applyHierarchyContexts();
}

void WorkbenchComponent::setAuditionPresentation(
    std::optional<std::string> definitionId,
    bool controlsCurrentProject)
{
    auditionDefinitionId_ = std::move(definitionId);
    auditionActive_ = auditionDefinitionId_.has_value();
    auditionControlsCurrentProject_ =
        auditionActive_ && controlsCurrentProject;
    soundDefinitionsPanel_->setAuditionState(
        auditionDefinitionId_, auditionControlsCurrentProject_);
}

void WorkbenchComponent::auditionSoundDefinition(std::string definitionId)
{
    startSoundLibraryAudition("project:" + std::move(definitionId));
}

[[nodiscard]] std::pair<std::string, std::string>
WorkbenchComponent::nextSoundDefinitionIdentity() const
{
    const auto& document =
        projectBridge_.editor().document();
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        const auto id = "sound" + std::to_string(ordinal);
        const auto exists = std::ranges::any_of(
            document.soundDefinitions,
            [&](const auto& definition) {
                return definition.definitionId == id;
            });
        if (!exists)
        {
            return {
                id,
                "Sound " + std::to_string(ordinal),
            };
        }
    }
}

void WorkbenchComponent::createSoundDefinition()
{
    const auto [definitionId, displayName] =
        nextSoundDefinitionIdentity();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = definitionId;
    definition.displayName = displayName;
    const auto result =
        projectBridge_.editor().addSoundDefinition(
            std::move(definition));
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    setStatus("Created " + juceString(displayName));
}

void WorkbenchComponent::renameSoundDefinition(std::string definitionId)
{
    if (definitionId.empty())
    {
        setStatus("Select a reusable Sound to rename");
        return;
    }
    const auto& definitions =
        projectBridge_.editor().document().soundDefinitions;
    const auto definition = std::ranges::find_if(
        definitions,
        [&](const auto& candidate) {
            return candidate.definitionId == definitionId;
        });
    if (definition == definitions.end())
    {
        setStatus("Selected reusable Sound no longer exists");
        return;
    }

    soundDialog_ = std::make_unique<juce::AlertWindow>(
        "Rename Reusable Sound",
        "Stable id: " + juceString(definitionId),
        juce::MessageBoxIconType::NoIcon,
        this);
    soundDialog_->addTextEditor(
        "soundName",
        juceString(definition->displayName),
        "Display name");
    soundDialog_->addButton(
        "Rename",
        1,
        juce::KeyPress(juce::KeyPress::returnKey));
    soundDialog_->addButton(
        "Cancel",
        0,
        juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    soundDialog_->enterModalState(
        true,
        juce::ModalCallbackFunction::create(
            [safeThis,
             definitionId = std::move(definitionId)](
                int result) {
                if (safeThis == nullptr
                    || safeThis->soundDialog_ == nullptr)
                {
                    return;
                }
                const auto displayName =
                    safeThis->soundDialog_
                        ->getTextEditor("soundName")
                        ->getText()
                        .toStdString();
                safeThis->soundDialog_.reset();
                safeThis->scheduleWorkbenchFocusRestoration();
                if (result != 1
                    || safeThis->renameSoundFileVariable(definitionId, displayName))
                    return;
                const auto renamed =
                    safeThis->projectBridge_.editor()
                        .renameSoundDefinition(
                            definitionId,
                            displayName);
                if (!renamed.ok())
                {
                    safeThis->setStatus(
                        juceString(renamed.message));
                    return;
                }
                safeThis->documentChanged();
                safeThis->setStatus(
                    "Renamed reusable Sound");
            }),
        false);
}

void WorkbenchComponent::deleteSoundDefinition(std::string definitionId)
{
    if (definitionId.empty())
    {
        setStatus("Select a reusable Sound to delete");
        return;
    }
    const auto result =
        projectBridge_.editor().removeSoundDefinition(
            definitionId);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    showRootGraph();
    setStatus("Deleted reusable Sound");
}

// When the command, by the menu it is in, tests the value
// typed into the field.
talas::app::FieldTestMoment WorkbenchComponent::fieldTestMomentOfCommand(juce::CommandID commandId)
{
    if (commandId == commandRevertSound)
        return talas::app::FieldTestMoment::Revert;
    if (commandId == commandEvaluateSelection)
        return talas::app::FieldTestMoment::Evaluate;
    // Pause and Play/Pause, the transport's here, hold or resume the Sound.
    if (commandId == commandPause || commandId == commandTogglePlayback)
        return talas::app::FieldTestMoment::OtherMenuOperation;
    juce::ApplicationCommandInfo command{commandId};
    getCommandInfo(commandId, command);
    if (command.categoryName == juceString(talas::app::talasMenuName(talas::app::TalasMenu::Action)))
        return talas::app::FieldTestMoment::ActionMenuOperation;
    if (command.categoryName == juceString(talas::app::talasMenuName(talas::app::TalasMenu::Info)))
        return talas::app::FieldTestMoment::InfoMenuOperation;
    return talas::app::FieldTestMoment::OtherMenuOperation;
}

// The signal flow of the Sound being edited.
[[nodiscard]] const talas::project::ProjectGraph* WorkbenchComponent::graphForContext(
    const std::optional<std::string>& definitionId) const
{
    return talas::app::soundEditorSignalFlow(projectBridge_.editor().document(), definitionId);
}

[[nodiscard]] const talas::project::ProjectNode* WorkbenchComponent::nodeForContext(
    const std::optional<std::string>& definitionId,
    std::string_view nodeId) const
{
    const auto* graph = graphForContext(definitionId);
    if (graph == nullptr)
        return nullptr;
    const auto node = std::ranges::find_if(
        graph->nodes,
        [&](const auto& candidate) {
            return candidate.instanceId == nodeId;
        });
    return node == graph->nodes.end() ? nullptr : &*node;
}

[[nodiscard]] std::string WorkbenchComponent::nextSoundInstanceId(
    const std::optional<std::string>& parentDefinitionId,
    std::string_view definitionId) const
{
    auto base = std::string(definitionId);
    if (base.empty())
        base = "sound";
    return talas::app::numberedCopyName(base + "Instance", [&](std::string_view candidate) {
        return nodeForContext(parentDefinitionId, candidate) != nullptr;
    });
}

void WorkbenchComponent::addSoundInstance(std::string definitionId)
{
    if (definitionId.empty())
    {
        setStatus("Select a reusable Sound to instantiate");
        return;
    }
    const auto* graph = graphForContext(activeGraphContext_);
    if (graph == nullptr)
    {
        setStatus("Active Structure context is unavailable");
        return;
    }
    const auto nodeCount = graph->nodes.size();
    const auto instanceId =
        nextSoundInstanceId(activeGraphContext_, definitionId);
    const auto position = talas::project::CanvasPosition{
        72 + static_cast<std::int32_t>(nodeCount % 3) * 210,
        64 + static_cast<std::int32_t>((nodeCount / 3) % 3) * 150,
    };
    const auto result = activeGraphContext_.has_value()
        ? projectBridge_.editor().addSoundInstanceToSoundDefinition(
              *activeGraphContext_,
              instanceId,
              definitionId,
              position)
        : projectBridge_.editor().addSoundInstance(
              instanceId,
              definitionId,
              position);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    selectNode(activeGraphContext_, instanceId);
    setStatus("Added reusable Sound instance");
}

void WorkbenchComponent::extractSelectedNodeToSound()
{
    if (activeGraphContext_.has_value())
    {
        setStatus("Extract to Sound currently operates in Project / Root");
        return;
    }
    if (selectedNodeIds_.empty())
    {
        setStatus("Select one or more nodes to extract as a Sound");
        return;
    }
    const auto [definitionId, displayName] =
        nextSoundDefinitionIdentity();
    const auto instanceId =
        nextSoundInstanceId(std::nullopt, definitionId);
    const auto result =
        projectBridge_.editor().extractNodesToSound(
            selectedNodeIds_,
            definitionId,
            displayName,
            instanceId);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    selectNode(instanceId);
    documentChanged();
    const auto& extractedDefinition =
        projectBridge_.editor().document().soundDefinitions.back();
    setStatus(
        "Extracted selection to "
        + juceString(displayName)
        + juceString(" · substituted ")
        + juce::String(static_cast<juce::int64>(
              extractedDefinition.inputs.size()))
        + " external input"
        + (extractedDefinition.inputs.size() == 1 ? "" : "s"));
}

std::optional<talas::project::ProjectNode> WorkbenchComponent::makePrototypeNode(
    const std::optional<std::string>& definitionId,
    std::string_view typeId) const
{
    const auto* descriptor = registry_.findNodeType(typeId);
    if (descriptor == nullptr)
        return std::nullopt;
    std::vector<talas::project::ProjectParameterExpression> parameters;
    parameters.reserve(descriptor->parameters.size());
    for (const auto& parameter : descriptor->parameters)
    {
        if (parameter.kind == talas::core::ParameterKind::Expression
            && parameter.defaultExpression.empty())
            continue;
        parameters.push_back(defaultParameter(parameter));
    }
    return talas::project::ProjectNode{
        nextNodeId(definitionId, *descriptor),
        descriptor->typeId,
        std::move(parameters),
    };
}

void WorkbenchComponent::dropItemAt(
    std::optional<std::string> definitionId,
    GraphDropItem item,
    talas::project::CanvasPosition position,
    GraphDropTarget target)
{
    if (!item.prototypeTypeId.empty())
    {
        dropPrototypeAt(
            std::move(definitionId), std::move(item.prototypeTypeId), position, std::move(target));
        return;
    }
    const auto soundId = item.soundDefinitionId;
    if (std::ranges::none_of(
            projectBridge_.editor().document().soundDefinitions,
            [&](const auto& definition) { return definition.definitionId == soundId; }))
    {
        setStatus("The dragged Sound is no longer in the project");
        return;
    }
    const auto instanceId = nextSoundInstanceId(definitionId, soundId);
    talas::project::ProjectNode node;
    node.instanceId = instanceId;
    node.soundDefinitionId = soundId;
    const auto scope = optionalStringView(definitionId);
    if (target.connection.has_value() && !target.inputPortId.has_value())
    {
        auto inputs = projectBridge_.editor().inputsTakingConnection(scope, node, *target.connection);
        if (inputs.size() > 1)
        {
            chooseInput("Place " + juceString(target.connection->fromNode) + " into which input of "
                    + juceString(soundId) + "?",
                std::move(inputs),
                [this, definitionId, item, position, target](std::string input) mutable {
                    target.inputPortId = std::move(input);
                    dropItemAt(std::move(definitionId), std::move(item), position, std::move(target));
                });
            return;
        }
        if (inputs.size() == 1)
            target.inputPortId = std::move(inputs.front());
    }
    if (!target.nodeId.empty() && !target.replaceAll.has_value()
        && confirmSourceReplacement(scope, target.nodeId, node, [this, definitionId, item, position, target]() mutable {
               target.replaceAll = false;
               dropItemAt(std::move(definitionId), std::move(item), position, std::move(target));
           }))
        return;
    if (!target.nodeId.empty() && !target.replaceAll.has_value() && replacingAsks(scope, target.nodeId, node))
    {
        chooseInput("Replace " + juceString(target.nodeId) + " alone, or with its inputs?", {"Replace", "Replace all"},
            [this, definitionId, item, position, target](std::string choice) mutable {
                target.replaceAll = choice == "Replace all";
                dropItemAt(std::move(definitionId), std::move(item), position, std::move(target));
            });
        return;
    }
    const auto result = target.connection.has_value()
        ? projectBridge_.editor().insertNodeOnConnection(
              scope, std::move(node), position, *target.connection, optionalStringView(target.inputPortId))
        : !target.nodeId.empty()
        ? projectBridge_.editor().replaceNode(scope, target.nodeId, std::move(node), target.replaceAll.value_or(false))
        : definitionId.has_value()
        ? projectBridge_.editor().addSoundInstanceToSoundDefinition(
              *definitionId, instanceId, soundId, position)
        : projectBridge_.editor().addSoundInstance(instanceId, soundId, position);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    selectNode(definitionId, instanceId);
    setStatus(target.connection.has_value()
            ? "Inserted the Sound " + juceString(soundId) + " into the line"
            : !target.nodeId.empty()
            ? "Replaced " + juceString(target.nodeId) + " with the Sound " + juceString(soundId)
            : "Added the Sound " + juceString(soundId));
}

void WorkbenchComponent::dropPrototypeAt(
    std::optional<std::string> definitionId,
    std::string typeId,
    talas::project::CanvasPosition position,
    GraphDropTarget target)
{
    if (talas::app::isPrototypeAssembly(typeId))
        return placePrototypeAssembly(definitionId, typeId, position, target);
    if (!target.connection.has_value() && target.nodeId.empty())
    {
        addPrototypeAt(std::move(definitionId), std::move(typeId), position);
        return;
    }
    const auto* descriptor = registry_.findNodeType(typeId);
    auto node = makePrototypeNode(definitionId, typeId);
    if (descriptor == nullptr || !node.has_value())
    {
        setStatus("Unknown prototype cannot be added");
        return;
    }
    const auto nodeId = node->instanceId;
    const auto scope = optionalStringView(definitionId);
    if (target.connection.has_value() && !target.inputPortId.has_value())
    {
        auto inputs = projectBridge_.editor().inputsTakingConnection(scope, *node, *target.connection);
        if (inputs.size() > 1)
        {
            chooseInput("Place " + juceString(target.connection->fromNode) + " into which input of the "
                    + juceString(descriptor->displayName) + "?",
                std::move(inputs),
                [this, definitionId, typeId, position, target](std::string input) mutable {
                    target.inputPortId = std::move(input);
                    dropPrototypeAt(std::move(definitionId), std::move(typeId), position, std::move(target));
                });
            return;
        }
    }
    if (!target.connection.has_value() && !target.replaceAll.has_value()
        && confirmSourceReplacement(scope, target.nodeId, *node, [this, definitionId, typeId, position, target]() mutable {
               target.replaceAll = false;
               dropPrototypeAt(std::move(definitionId), std::move(typeId), position, std::move(target));
           }))
        return;
    if (!target.connection.has_value() && !target.replaceAll.has_value() && replacingAsks(scope, target.nodeId, *node))
    {
        chooseInput("Replace " + juceString(target.nodeId) + " alone, or with its inputs?", {"Replace", "Replace all"},
            [this, definitionId, typeId, position, target](std::string choice) mutable {
                target.replaceAll = choice == "Replace all";
                dropPrototypeAt(std::move(definitionId), std::move(typeId), position, std::move(target));
            });
        return;
    }
    const auto result = target.connection.has_value()
        ? projectBridge_.editor().insertNodeOnConnection(
              scope, std::move(*node), position, *target.connection, optionalStringView(target.inputPortId))
        : projectBridge_.editor().replaceNode(
              scope, target.nodeId, std::move(*node), target.replaceAll.value_or(false));
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    selectNode(definitionId, nodeId);
    documentChanged();
    auto message = target.connection.has_value()
        ? "Inserted " + juceString(descriptor->displayName) + " between "
            + juceString(target.connection->fromNode) + " and "
            + juceString(target.connection->toNode)
        : "Replaced " + juceString(target.nodeId) + " with "
            + juceString(descriptor->displayName);
    if (!result.message.empty())
        message += juceString(" · ") + juceString(result.message);
    setStatus(message);
}

void WorkbenchComponent::showFakeKeyboard()
{
    if (fakeKeyboardPanel_ == nullptr)
    {
        fakeKeyboardPanel_ = makeFakeKeyboardPanel(
            [this](const talas::app::OnScreenKeyboardNote& note) {
                playOnScreenKeyboardNote(note);
            });
    }
    // A Workbench that is not on screen (a smoke run) keeps the keys
    // without opening a window.
    if (isShowing())
    {
        if (fakeKeyboardWindow_ == nullptr)
            fakeKeyboardWindow_ = makeFakeKeyboardWindow(*fakeKeyboardPanel_);
        fakeKeyboardWindow_->setVisible(true);
        fakeKeyboardWindow_->toFront(true);
    }
    setStatus("Fake Keyboard: click keys to play MIDI notes into the live Sound");
}

void WorkbenchComponent::showLiveGridEditor()
{
    if (liveGridEditorPanel_ == nullptr)
    {
        liveGridEditorPanel_ = makeLiveGridEditorPanel(
            projectBridge_,
            [this] { documentChanged(); },
            [this](const juce::String& status) { setStatus(status); },
            [this] {
                return projectBridge_.editor().document()
                    .deviceSettings.midiInputDeviceIdentifiers;
            });
        liveGridEditorPanel_->setGridHandlers(
            [this](const std::string& set, bool on) { return setLiveGridOn(set, on); },
            [this](const std::string& set, const std::string& cell) {
                return performancePanel_ != nullptr && performancePanel_->activateLiveGridCell(set, cell);
            },
            [this](const std::string& sound) { showSoundDefinition(sound); });
    }
    liveGridEditorPanel_->refresh();
    if (isShowing())
    {
        if (liveGridEditorWindow_ == nullptr)
        {
            liveGridEditorWindow_ = makeToolWindow(
                "Live Grid Editor", *liveGridEditorPanel_);
        }
        liveGridEditorWindow_->setVisible(true);
        liveGridEditorWindow_->toFront(true);
    }
    setStatus("Live Grid Editor: choose what each box plays from its menu");
}

void WorkbenchComponent::showAnalysisTool(AnalysisToolPanel::Kind kind)
{
    const auto spectral = kind == AnalysisToolPanel::Kind::Spectral;
    const auto ga = kind == AnalysisToolPanel::Kind::Ga;
    auto& panel = spectral ? spectralAnalysisPanel_ : ga ? gaAnalysisPanel_ : reAnalysisPanel_;
    auto& window = spectral ? spectralAnalysisWindow_ : ga ? gaAnalysisWindow_ : reAnalysisWindow_;
    if (panel == nullptr)
    {
        panel = makeAnalysisToolPanel(
            kind,
            [this](const juce::String& status) { setStatus(status); },
            [this](AnalysisToolPanel::Completed completed) {
                addAnalysisExample(completed);
            },
            // Spectral Analysis's auditions loop;
            // RE and GA Analysis play the file chosen once.
            [this, kind](std::vector<double> samples, double sampleRate) {
                if (samples.empty())
                    return stopSampleBuffer();
                const auto loops = talas::app::analysisToolPreviewLoops(kind == AnalysisToolPanel::Kind::Spectral ? talas::app::AnalysisToolKind::Spectral
                        : kind == AnalysisToolPanel::Kind::Ga ? talas::app::AnalysisToolKind::Ga
                                                              : talas::app::AnalysisToolKind::Re);
                playSampleBuffer({std::move(samples)}, sampleRate, loops);
                setStatus(loops ? "Spectral Analysis: auditioning" : "Playing the file chosen once");
            });
    }
    if (isShowing())
    {
        if (window == nullptr)
        {
            window = makeToolWindow(
                spectral ? "Spectral Analysis" : ga ? "GA Analysis" : "RE Analysis", *panel);
        }
        window->setVisible(true);
        window->toFront(true);
    }
    setStatus(spectral
            ? juce::String{"Spectral Analysis: choose a sample, set the analysis, create the spectrum file"}
            : ga ? juce::String{"GA Analysis: choose a sample, set the harmonics and waveforms, create the GA file"}
                 : juce::String{"RE Analysis: choose a sample, set the analysis, create the RE file"});
}

void WorkbenchComponent::showCdpProcessTool()
{
    if (cdpProcessPanel_ == nullptr)
    {
        cdpProcessPanel_ = makeCdpProcessToolPanel(
            [this](const juce::String& status) { setStatus(status); },
            [this](CdpProcessToolPanel::Completed completed) { addCdpProcessExample(completed); });
    }
    if (isShowing())
    {
        if (cdpProcessWindow_ == nullptr)
            cdpProcessWindow_ = makeToolWindow("CDP Process to Sample", *cdpProcessPanel_);
        cdpProcessWindow_->setVisible(true);
        cdpProcessWindow_->toFront(true);
    }
    setStatus("CDP Process to Sample: choose samples and a process, then process to a sample");
}

// The processed sample is left as an example Sound that plays it, as the
// analysis tools leave theirs.
void WorkbenchComponent::addCdpProcessExample(const CdpProcessToolPanel::Completed& completed)
{
    auto example = talas::app::cdpProcessExampleSound(projectBridge_.editor().document(), completed.output);
    const auto definitionId = example.definition.definitionId;
    const auto displayName = example.definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(example.definition)); !added.ok())
    {
        setStatus(completed.summary + " · the example Sound was refused: " + juceString(added.message));
        return;
    }
    documentChanged();
    for (const auto& resource : example.resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    setStatus(completed.summary + " · added the example Sound " + juceString(displayName));
}

// Notes take the path a MIDI input's do: to the playing Sound, or to the
// Samples workspace's key selection while it routes MIDI.
void WorkbenchComponent::playOnScreenKeyboardNote(
    const talas::app::OnScreenKeyboardNote& note)
{
    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const auto channel = std::clamp(note.channel, 1, 16);
    const auto key = static_cast<std::uint8_t>(std::clamp(note.note, 0, 127));
    if (sampleMidiRoutingEnabled_.load(std::memory_order_acquire))
    {
        if (note.velocity > 0)
        {
            handleSampleMapMidiNoteOn(
                channel,
                key,
                static_cast<std::uint8_t>(std::clamp(note.velocity, 1, 127)),
                now);
        }
        else
            handleSampleMapMidiNoteOff(channel, key, now);
        return;
    }
    if (note.velocity > 0)
        (void)projectBridge_.enqueueMidiNoteOn(channel, key, note.velocity, now);
    else
        (void)projectBridge_.enqueueMidiNoteOff(channel, key, now);
}

juce::String WorkbenchComponent::graphClipboardText() const
{
    return persistentStateEnabled_
        ? juce::SystemClipboard::getTextFromClipboard()
        : isolatedGraphClipboard_;
}

void WorkbenchComponent::setGraphClipboardText(const juce::String& text)
{
    if (persistentStateEnabled_)
        juce::SystemClipboard::copyTextToClipboard(text);
    else
        isolatedGraphClipboard_ = text;
}

bool WorkbenchComponent::copySelectedNodes(std::vector<std::string> nodeIds)
{
    if (nodeIds.empty())
        nodeIds = selectedNodeIds_;
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        || nodeIds.empty())
    {
        setStatus("No active graph selection to copy");
        return false;
    }
    const auto copy = talas::project::copyProjectGraphFragment(
        projectBridge_.editor().document(),
        optionalStringView(activeGraphContext_),
        nodeIds);
    if (!copy.fragment.has_value())
    {
        setStatus(juceString(copy.message));
        return false;
    }
    setGraphClipboardText(juceString(
        talas::project::serializeProjectGraphFragment(*copy.fragment)));
    setStatus(
        "Copied "
        + juce::String(static_cast<int>(copy.fragment->nodes.size()))
        + (copy.fragment->nodes.size() == 1 ? " node" : " nodes"));
    return true;
}

void WorkbenchComponent::cutSelectedNodes(std::vector<std::string> nodeIds)
{
    if (nodeIds.empty())
        nodeIds = selectedNodeIds_;
    if (!copySelectedNodes(nodeIds))
        return;
    requestDeleteSelectedNodes(activeGraphContext_, std::move(nodeIds), false);
}

void WorkbenchComponent::pasteGraphNodes(
    std::optional<talas::project::CanvasPosition> position)
{
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        || graphForContext(activeGraphContext_) == nullptr)
    {
        setStatus("The active workspace does not expose a Structure graph");
        return;
    }
    const auto parsed = talas::project::parseProjectGraphFragment(
        graphClipboardText().toStdString());
    if (!parsed.fragment.has_value())
    {
        setStatus(juceString(parsed.message));
        return;
    }
    // One Sound pasted while one is selected
    // replaces it, asking Replace or Replace all as a drop does.
    if (!position.has_value() && parsed.fragment->nodes.size() == 1 && parsed.fragment->connections.empty()
        && selectedNodeIds_.size() == 1)
    {
        const auto* graph = graphForContext(activeGraphContext_);
        auto replacement = parsed.fragment->nodes.front();
        const auto taken = [&](std::string_view id) {
            return std::ranges::any_of(graph->nodes, [&](const auto& node) { return node.instanceId == id; });
        };
        const auto base = replacement.instanceId;
        for (auto copy = 1; taken(replacement.instanceId); ++copy)
            replacement.instanceId = base + (copy == 1 ? "-copy" : "-copy" + std::to_string(copy));
        pasteReplacing(selectedNodeIds_.front(), std::move(replacement), std::nullopt);
        return;
    }
    // Without a pointer position the copies land beside where the Sounds
    // were copied from, as Duplicate places them.
    if (!position.has_value())
    {
        auto origin = parsed.fragment->positions.front();
        for (const auto& placed : parsed.fragment->positions)
        {
            origin.x = std::min(origin.x, placed.x);
            origin.y = std::min(origin.y, placed.y);
        }
        position = talas::project::CanvasPosition{
            origin.x < std::numeric_limits<std::int32_t>::max() - 32 ? origin.x + 32 : origin.x,
            origin.y < std::numeric_limits<std::int32_t>::max() - 32 ? origin.y + 32 : origin.y,
        };
    }
    std::vector<std::string> pasted;
    const auto result = projectBridge_.editor().pasteGraphFragment(
        optionalStringView(activeGraphContext_),
        *parsed.fragment,
        *position,
        pasted);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    if (!pasted.empty())
        publishGraphSelection(activeGraphContext_, pasted, pasted.front());
    setStatus(
        "Pasted "
        + juce::String(static_cast<int>(pasted.size()))
        + (pasted.size() == 1 ? " node" : " nodes")
        + " as one project edit");
}

void WorkbenchComponent::pasteReplacing(
    std::string replacedNodeId, talas::project::ProjectNode replacement, std::optional<bool> replaceAll)
{
    const auto scope = optionalStringView(activeGraphContext_);
    if (!replaceAll.has_value()
        && confirmSourceReplacement(scope, replacedNodeId, replacement,
            [this, replacedNodeId, replacement] { pasteReplacing(replacedNodeId, replacement, false); }))
        return;
    if (!replaceAll.has_value() && replacingAsks(scope, replacedNodeId, replacement))
    {
        chooseInput("Replace " + juceString(replacedNodeId) + " alone, or with its inputs?", {"Replace", "Replace all"},
            [this, replacedNodeId, replacement](std::string choice) {
                pasteReplacing(replacedNodeId, replacement, choice == "Replace all");
            });
        return;
    }
    const auto nodeId = replacement.instanceId;
    const auto result = projectBridge_.editor().replaceNode(
        scope, replacedNodeId, std::move(replacement), replaceAll.value_or(false));
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    selectNode(activeGraphContext_, nodeId);
    setStatus("Replaced " + juceString(replacedNodeId) + " with the pasted " + juceString(nodeId));
}

void WorkbenchComponent::addPrototypeAt(
    std::optional<std::string> definitionId,
    std::string typeId,
    talas::project::CanvasPosition position)
{
    if (talas::app::isPrototypeAssembly(typeId))
        return placePrototypeAssembly(definitionId, typeId, position, {});
    const auto* descriptor = registry_.findNodeType(typeId);
    auto prototypeNode = makePrototypeNode(definitionId, typeId);
    if (descriptor == nullptr || !prototypeNode.has_value())
    {
        setStatus("Unknown prototype cannot be added");
        return;
    }

    const auto nodeId = prototypeNode->instanceId;
    auto node = std::move(*prototypeNode);
    const auto result = definitionId.has_value()
        ? projectBridge_.editor().addNodeToSoundDefinition(
              *definitionId,
              std::move(node),
              position)
        : projectBridge_.editor().addNode(
              std::move(node),
              position);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }

    selectNode(definitionId, nodeId);
    documentChanged();
    auto message = "Added " + juceString(descriptor->displayName);
    if (!result.message.empty())
        message += juceString(" · ") + juceString(result.message);
    setStatus(message);
}

void WorkbenchComponent::addPrototypeAt(
    std::string typeId,
    talas::project::CanvasPosition position)
{
    addPrototypeAt(
        std::nullopt,
        std::move(typeId),
        position);
}

std::string WorkbenchComponent::nextNodeId(
    const std::optional<std::string>& definitionId,
    const talas::core::NodeTypeDescriptor& descriptor) const
{
    std::string base = descriptor.typeId;
    constexpr std::string_view prefix = "talas.";
    if (base.starts_with(prefix))
        base.erase(0, prefix.size());
    std::replace(base.begin(), base.end(), '.', '_');
    return talas::app::numberedCopyName(base, [&](std::string_view candidate) {
        return nodeForContext(definitionId, candidate) != nullptr;
    });
}

void WorkbenchComponent::refreshPerformancePanelSelection()
{
    const auto rootSelection =
        talas::app::soundWorkbenchSessionSelection(
            workspaceSession_,
            std::nullopt);
    performancePanel_->setSelectionContext(
        rootSelection,
        talas::app::snapshotWorkbenchCaptureContext(
            projectBridge_.editor().document(),
            selectedNodeId_,
            optionalStringView(activeGraphContext_),
            rootSelection));
}

void WorkbenchComponent::focusPerformanceControlInStructure(
    const talas::app::SoundPerformanceControlIdentity& identity)
{
    const auto target = talas::app::soundPerformanceStructureTarget(
        projectBridge_.editor().document(),
        identity);
    if (!target.has_value())
    {
        setStatus("Performance control no longer has an exact Structure owner; "
                  "refresh the current project state");
        return;
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    showRootGraph();
    selectNode(target->instanceId);
    inspectorPanel_->focusParameter(target->parameterId);
    setStatus("Focused Structure owner " + juceString(target->instanceId) +
              " / " + juceString(target->parameterId));
}

std::optional<std::string>
WorkbenchComponent::runE4HierarchyNavigationSmoke(
    const std::string& definitionId,
    const std::string& instanceId,
    const std::string& scopedSelection,
    const talas::project::WorkspaceSessionState& restoreSession)
{
    const auto fail = [](bool condition, std::string message)
        -> std::optional<std::string> {
        return condition
            ? std::nullopt
            : std::optional<std::string>{std::move(message)};
    };
    const auto projectBeforeFixture = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const std::string childDefinitionId = "e4NestedChild";
    talas::project::ProjectSoundDefinition childDefinition;
    childDefinition.definitionId = childDefinitionId;
    childDefinition.displayName = "E4 Nested Child";
    const auto addedChild =
        projectBridge_.editor().addSoundDefinition(std::move(childDefinition));
    if (!addedChild.ok())
        return "E4 nested Sound fixture failed: " + addedChild.message;
    const auto addedChildInstance =
        projectBridge_.editor().addSoundInstanceToSoundDefinition(
            definitionId,
            "e4ChildInstance",
            childDefinitionId,
            {320, 96});
    if (!addedChildInstance.ok())
        return "E4 nested Sound instance fixture failed: " +
               addedChildInstance.message;
    documentChanged();

    const auto beforeProject = talas::project::serializeProjectDocument(
        projectBridge_.editor().document());
    const auto beforeDirty = editBoundary_.isDirty(
        projectBridge_.editor().document());
    const auto beforeStage = projectBridge_.latestStageGeneration();
    const auto beforePlaying = transportPlaying_;
    const auto beforeCanUndo = projectBridge_.editor().canUndo();
    const auto beforeCanRedo = projectBridge_.editor().canRedo();

    workspaceSession_.paneSplit = 0.63;
    workspaceSession_.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::TopBottom;
    workspaceSession_.primaryPaneFirst = false;
    handleSoundAction(
        SoundDefinitionsPanel::Action::ToggleBookmark,
        definitionId);
    const auto bookmarks =
        talas::project::workspaceSessionBookmarkedDefinitionIds(
            workspaceSession_);
    showRootGraph();
    workspaceSession_.paneSplit = 0.41;
    workspaceSession_.structureSplitOrientation =
        talas::project::WorkspaceStructureSplitOrientation::LeftRight;
    workspaceSession_.primaryPaneFirst = true;
    handleSoundAction(
        SoundDefinitionsPanel::Action::RestoreBookmark,
        definitionId);
    if (auto error = fail(
            bookmarks == std::vector<std::string>{definitionId}
                && hierarchyPath_ == std::vector<std::string>{definitionId}
                && secondaryGraphContext_
                    == std::optional<std::string>{definitionId}
                && std::abs(workspaceSession_.paneSplit - 0.63) < 0.001
                && workspaceSession_.structureSplitOrientation
                    == talas::project::WorkspaceStructureSplitOrientation::TopBottom
                && !workspaceSession_.primaryPaneFirst
                && selectedNodeId_ == scopedSelection,
            "E4 hierarchy bookmark did not restore path, split configuration, pane order, and retained per-scope selection"))
    {
        return error;
    }
    openChildDefinition(definitionId, childDefinitionId);
    if (auto error = fail(
            hierarchyPath_ ==
                    std::vector<std::string>{definitionId, childDefinitionId}
                && primaryGraphContext_ ==
                    std::optional<std::string>{definitionId}
                && secondaryGraphContext_ ==
                    std::optional<std::string>{childDefinitionId}
                && activeGraphContext_ ==
                    std::optional<std::string>{childDefinitionId},
            "E4 nested Sound did not open in the secondary pane opposite its exact parent"))
    {
        return error;
    }
    handleSoundAction(
        SoundDefinitionsPanel::Action::NavigateBreadcrumb,
        definitionId);
    if (auto error = fail(
            hierarchyPath_ == std::vector<std::string>{definitionId}
                && !primaryGraphContext_.has_value()
                && secondaryGraphContext_ ==
                    std::optional<std::string>{definitionId}
                && activeGraphContext_ ==
                    std::optional<std::string>{definitionId},
            "E4 parent breadcrumb did not truncate the child scope while retaining the parent as active"))
    {
        return error;
    }
    if (auto error = fail(
            soundDefinitionsPanel_
                ->hierarchyNavigationIsAccessibleForSmoke(),
            "E4 definition browser and breadcrumb navigation lacked keyboard-accessible explicit controls"))
    {
        return error;
    }
    soundDefinitionsPanel_->setNavigationPath(std::vector<std::string>(
        talas::project::maximumWorkspaceNavigationDepth,
        definitionId));
    if (auto error = fail(
            soundDefinitionsPanel_
                ->hierarchyNavigationIsAccessibleForSmoke(),
            "E4 maximum-depth breadcrumb navigation produced an inaccessible or empty segment"))
    {
        return error;
    }
    soundDefinitionsPanel_->setNavigationPath(hierarchyPath_);
    handleSoundAction(
        SoundDefinitionsPanel::Action::NavigateBreadcrumb,
        {});
    if (auto error = fail(
            hierarchyPath_.empty()
                && !secondaryGraphContext_.has_value()
                && !activeGraphContext_.has_value(),
            "E4 Root breadcrumb did not deterministically clear the visible hierarchy"))
    {
        return error;
    }
    handleSoundAction(
        SoundDefinitionsPanel::Action::OpenDefinition,
        definitionId);
    if (auto error = fail(
            hierarchyPath_ == std::vector<std::string>{definitionId}
                && secondaryGraphContext_ ==
                    std::optional<std::string>{definitionId}
                && activeGraphContext_ ==
                    std::optional<std::string>{definitionId}
                && workspaceSession_.activeSoundDefinitionId ==
                    std::optional<std::string>{definitionId},
            "E4 Show in Structure did not make the exact browsed definition the active opposite-pane scope"))
    {
        return error;
    }
    showRootGraph();
    selectNode(instanceId);
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    if (auto error = fail(
            performancePanel_->controlCount() == 2
                && performanceCanvas_->visibleItemCountForSmoke() > 0
                && performanceCanvas_->controlsAccessibleForSmoke()
                && performancePanel_->focusFirstControlInStructureForSmoke()
                && workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch
                && !activeGraphContext_.has_value()
                && selectedNodeId_ == instanceId
                && inspectorPanel_->focusedParameterForSmoke("gainScale"),
            "E4 Performance control did not focus its exact root Sound instance and exposed parameter in Structure"))
    {
        return error;
    }
    if (auto error = fail(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document()) == beforeProject
                && editBoundary_.isDirty(
                       projectBridge_.editor().document()) == beforeDirty
                && projectBridge_.latestStageGeneration() == beforeStage
                && transportPlaying_ == beforePlaying
                && projectBridge_.editor().canUndo() == beforeCanUndo
                && projectBridge_.editor().canRedo() == beforeCanRedo,
            "E4 navigation or bookmark operations changed project bytes, dirty state, history availability, runtime generation, or transport"))
    {
        return error;
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    const auto beforePerformanceLayout =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    const auto performanceScaleBaseline =
        workspaceSession_.accessibility;
    const auto addedSecondPerformanceControl =
        performancePanel_->addFirstSoundControlToCanvasForSmoke();
    workspaceSession_.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent150;
    applyAccessibilityPreferences();
    performanceCanvas_->refresh();
    const auto scaledPerformanceLayoutCoherent =
        performanceCanvas_->visibleItemCountForSmoke() > 1
        && performanceCanvas_->scaledLayoutCoherentForSmoke();
    workspaceSession_.accessibility = performanceScaleBaseline;
    applyAccessibilityPreferences();
    performanceCanvas_->refresh();
    if (auto error = fail(
            addedSecondPerformanceControl
                && performanceCanvas_->visibleItemCountForSmoke() > 1
                && scaledPerformanceLayoutCoherent
                && performancePanel_
                       ->selectedPerformanceItemEditorAccessibleForSmoke()
                && projectBridge_.latestStageGeneration() == beforeStage
                && projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforePerformanceLayout,
            "E5 exact Sound control authoring did not remain portable, selected, stage-preserving, and one-undo"))
    {
        return error;
    }
    performanceCanvas_->refresh();
    if (auto error = fail(
            performanceCanvas_->addOutputMeterForSmoke()
                && performanceCanvas_->visibleItemCountForSmoke() > 1
                && projectBridge_.latestStageGeneration() == beforeStage
                && projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforePerformanceLayout,
            "E5 output meter authoring did not remain portable, stage-preserving, and one-undo"))
    {
        return error;
    }
    performanceCanvas_->refresh();
    if (auto error = fail(
            performanceCanvas_->setCompactForSmoke(true)
                && performanceCanvas_->setCompactForSmoke(false)
                && performanceCanvas_->moveFirstControlForSmoke({413, 237})
                && performancePanel_
                       ->selectedPerformanceItemEditorAccessibleForSmoke()
                && projectBridge_.latestStageGeneration() == beforeStage
                && projectBridge_.editor().undo().ok()
                && talas::project::serializeProjectDocument(
                       projectBridge_.editor().document())
                    == beforePerformanceLayout
                && projectBridge_.latestStageGeneration() == beforeStage,
            "E5 Performance compact/layout gesture did not preserve Structure bytes, one-undo restoration, and prepared stage generation"))
    {
        return error;
    }
    performanceCanvas_->refresh();
    const auto removedChildInstance =
        projectBridge_.editor().removeNodeFromSoundDefinition(
            definitionId,
            "e4ChildInstance");
    if (!removedChildInstance.ok())
    {
        return "E4 nested Sound instance cleanup failed: " +
               removedChildInstance.message;
    }
    const auto removedChild =
        projectBridge_.editor().removeSoundDefinition(childDefinitionId);
    if (!removedChild.ok())
        return "E4 nested Sound cleanup failed: " + removedChild.message;
    documentChanged();
    if (auto error = fail(
            talas::project::serializeProjectDocument(
                projectBridge_.editor().document()) == projectBeforeFixture,
            "E4 nested hierarchy smoke fixture did not restore the exact original project bytes"))
    {
        return error;
    }
    workspaceSession_ = restoreSession;
    applyWorkspaceSessionProjection(true);
    return std::nullopt;
}

void WorkbenchComponent::selectNode(std::optional<std::string> definitionId,
    std::string nodeId,
    bool extendSelection)
{
    const auto previouslySelected = selectedNodeId_;
    const auto previousContext = activeGraphContext_;
    if (definitionId != activeGraphContext_ || !extendSelection)
        selectedNodeIds_.clear();
    activeGraphContext_ = definitionId;
    if (nodeId.empty())
    {
        selectedNodeIds_.clear();
    }
    else if (extendSelection)
    {
        const auto found = std::ranges::find(
            selectedNodeIds_,
            nodeId);
        if (found == selectedNodeIds_.end())
            selectedNodeIds_.push_back(nodeId);
        else
            selectedNodeIds_.erase(found);
    }
    else
    {
        selectedNodeIds_.push_back(nodeId);
    }
    selectedNodeId_ = selectedNodeIds_.empty()
        ? std::string{}
        : (std::ranges::find(selectedNodeIds_, nodeId)
                   != selectedNodeIds_.end()
               ? nodeId
               : selectedNodeIds_.back());
    workspaceSession_.activeSoundDefinitionId = definitionId;
    // Revert goes back to the parameters the Sound had when it was opened
    //.
    if (!selectedNodeId_.empty()
        && (!revertSnapshot_.has_value() || selectedNodeId_ != previouslySelected || definitionId != previousContext))
    {
        const auto& document = projectBridge_.editor().document();
        const talas::project::ProjectGraph* graph = &document.graph;
        if (definitionId.has_value())
        {
            const auto definition = std::ranges::find(
                document.soundDefinitions, *definitionId, &talas::project::ProjectSoundDefinition::definitionId);
            graph = definition == document.soundDefinitions.end() ? nullptr : &definition->graph;
        }
        const auto node = graph == nullptr
            ? std::vector<talas::project::ProjectNode>::const_iterator{}
            : std::ranges::find(graph->nodes, selectedNodeId_, &talas::project::ProjectNode::instanceId);
        if (graph != nullptr && node != graph->nodes.end())
            revertSnapshot_ = RevertSnapshot{definitionId, selectedNodeId_, node->parameters};
    }
    talas::app::selectSoundWorkbenchSessionNode(
        workspaceSession_,
        definitionId,
        selectedNodeId_);
    if (definitionId == primaryGraphContext_)
        graphCanvas_->setSelectedNodeIds(
            selectedNodeIds_,
            selectedNodeId_);
    if (definitionId == secondaryGraphContext_)
        secondaryGraphCanvas_->setSelectedNodeIds(
            selectedNodeIds_,
            selectedNodeId_);
    const auto* selected = nodeForContext(
        definitionId,
        selectedNodeId_);
    analysisPanel_->setSelectedNode(
        selectedNodeId_,
        selected != nullptr ? std::string_view(selected->typeId) : std::string_view{});
    inspectorPanel_->setSoundDefinitionContext(definitionId);
    inspectorPanel_->setSelectedNodeId(selectedNodeId_);
    refreshPreparedGraphPresentation();
    refreshPerformancePanelSelection();
    refreshGraphContextPresentation();
    commandManager_.commandStatusChanged();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::publishGraphSelection(
    std::optional<std::string> definitionId,
    std::vector<std::string> nodeIds,
    std::string primaryNodeId)
{
    const auto* graph = graphForContext(definitionId);
    std::unordered_set<std::string> accepted;
    if (graph == nullptr)
        nodeIds.clear();
    else
    {
        std::unordered_set<std::string> available;
        available.reserve(graph->nodes.size());
        for (const auto& node : graph->nodes)
            available.insert(node.instanceId);
        accepted.reserve(nodeIds.size());
        std::erase_if(nodeIds, [&](const auto& nodeId) {
            if (nodeId.empty() || !available.contains(nodeId))
            {
                return true;
            }
            return !accepted.insert(nodeId).second;
        });
    }
    if (!accepted.contains(primaryNodeId))
    {
        primaryNodeId = nodeIds.empty()
            ? std::string{}
            : nodeIds.front();
    }
    activeGraphContext_ = definitionId;
    selectedNodeIds_ = std::move(nodeIds);
    selectedNodeId_ = std::move(primaryNodeId);
    workspaceSession_.activeSoundDefinitionId = definitionId;
    talas::app::selectSoundWorkbenchSessionNode(
        workspaceSession_, definitionId, selectedNodeId_);
    if (definitionId == primaryGraphContext_)
        graphCanvas_->setSelectedNodeIds(selectedNodeIds_, selectedNodeId_);
    if (definitionId == secondaryGraphContext_)
        secondaryGraphCanvas_->setSelectedNodeIds(
            selectedNodeIds_, selectedNodeId_);
    const auto* selected = nodeForContext(definitionId, selectedNodeId_);
    analysisPanel_->setSelectedNode(
        selectedNodeId_,
        selected != nullptr
            ? std::string_view(selected->typeId)
            : std::string_view{});
    inspectorPanel_->setSoundDefinitionContext(definitionId);
    inspectorPanel_->setSelectedNodeId(selectedNodeId_);
    refreshPreparedGraphPresentation();
    refreshPerformancePanelSelection();
    refreshGraphContextPresentation();
    commandManager_.commandStatusChanged();
    noteWorkspaceSessionChanged();
}

void WorkbenchComponent::selectNode(std::string nodeId)
{
    selectNode(std::nullopt, std::move(nodeId));
}

void WorkbenchComponent::documentChanged(bool refreshVirtualControls)
{
    fileOperationCoordinator_.advanceAuthority();
    scoreClipArtifactStateCache_.clear();
    invalidateAnalysisOutputTakeForProjectAuthorityChange();
    projectBridge_.stopControllerRecording(
        talas::project::ControllerRecordingTerminal::StageChanged);
    invalidateComponentReplacementForDocumentChange();
    stopSoundLibraryAudition();
    stopAnalysisAudition();
    stopPartialTrackPreview();
    const auto restoringSampleMapAudition = sampleMapAuditionActive_;
    if (sampleMapAuditionActive_
        || sampleMapAuditionPresentation_
            == SampleMapAuditionPresentation::Preparing)
    {
        stopSampleMapAudition();
    }
    if (!restoringSampleMapAudition)
        currentDocumentStaged_ = false;
    assetWatchBaseline_ = {};
    pendingAssetWatchStatus_.clear();
    pendingAssetReplacementId_.reset();
    publishCompileDiagnostics({});
    if (refreshVirtualControls)
    {
        virtualControlSurface_->cancelMidiLearn();
        stagedMidiRoutesValid_ = false;
        stagedVirtualControls_.clear();
    }
    else if (stagedMidiRoutesValid_)
    {
        stagedVirtualControls_ = projectBridge_.editor().document().virtualControls;
    }
    rebuildMidiRuntimeRoutes();
    rebuildOscIngress();
    if (refreshVirtualControls)
    {
        graphCanvas_->refreshModel();
        secondaryGraphCanvas_->refreshModel();
        inspectorPanel_->refresh();
        soundDefinitionsPanel_->refresh(
            projectBridge_.editor().document());
        if (soundFilePanel_ != nullptr)
            soundFilePanel_->refresh();
        refreshSoundLibraryProjection();
        talas::project::reconcileWorkspaceSessionState(
            workspaceSession_,
            projectBridge_.editor().document());
        soundDefinitionsPanel_->setBookmarks(
            talas::project::workspaceSessionBookmarkedDefinitionIds(
                workspaceSession_));
        refreshPerformancePanelSelection();
        performanceCanvas_->refresh();
        timelinePanel_->refresh();
        refreshPartialTrackEditorEdits();
        virtualControlSurface_->refresh();
    }
    refreshAnalysisWorkspace();
    refreshSampleMapWorkspace();
    if (liveGridEditorPanel_ != nullptr)
        liveGridEditorPanel_->refresh();
    undoButton_.setEnabled(projectBridge_.editor().canUndoIn(frontHistoryWindow()));
    redoButton_.setEnabled(projectBridge_.editor().canRedoIn(frontHistoryWindow()));
    commandManager_.commandStatusChanged();
    updateRecoveryAfterDocumentChange();
    refreshPreparedGraphPresentation();
    refreshWindowTitle();
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToSessionV1()
{
    if (projectBridge_.editor().document().expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV4)
    {
        requestProjectExpressionsUpgradeToValV1();
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV3)
    {
        requestProjectExpressionsUpgradeToVariablesV4();
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        requestProjectExpressionsUpgradeToVoiceV1();
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        == talas::expr::ExpressionLanguageVersion::VariablesV1)
    {
        requestProjectExpressionsUpgradeToVariablesV2();
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        == talas::expr::ExpressionLanguageVersion::SessionV1)
    {
        requestProjectExpressionsUpgradeToVariablesV1();
        return;
    }
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    const auto language =
        projectBridge_.editor().document().expressionLanguage;
    if (language == talas::expr::ExpressionLanguageVersion::SessionV1
        || (language != talas::expr::ExpressionLanguageVersion::LegacyV1
            && language
                != talas::expr::ExpressionLanguageVersion::PureV2
            && language
                != talas::expr::ExpressionLanguageVersion::PureV3
            && language
                != talas::expr::ExpressionLanguageVersion::PureV4))
    {
        setStatus("Expression upgrade is no longer available");
        return;
    }

    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::bad_alloc&)
    {
        setStatus("Expression upgrade unavailable: project snapshot allocation failed");
        return;
    }
    catch (const std::length_error&)
    {
        setStatus("Expression upgrade unavailable: project snapshot is too large");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Session state v1?")
        .withMessage(
            "Upgrade this project to format 26 and Session state v1?\n\n"
            "The compatibility audit is run again before committing. "
            "Only the format and expression-language authority change, "
            "one Undo restores the exact prior project and profile, and the graph "
            "must be staged again. Runtime expression state is session-only: it is "
            "not saved in the project or in snapshots.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Expression upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Expression upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::bad_alloc&)
            {
                safeThis->setStatus(
                    "Expression upgrade rejected: project snapshot allocation failed");
                return;
            }
            catch (const std::length_error&)
            {
                safeThis->setStatus(
                    "Expression upgrade rejected: project snapshot is too large");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToSessionV1();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToSessionV1()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToSessionV1();
    if (!result.ok())
    {
        setStatus(
            "Expression upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        juceString(
            "Upgraded project to Session state v1 as one Undo step · fresh staging required"));
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToVariablesV1()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        != talas::expr::ExpressionLanguageVersion::SessionV1)
    {
        setStatus("Variables v1 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Variables v1 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Variables v1?")
        .withMessage(
            "Upgrade this project to format 29 and Variables v1?\n\n"
            "The compatibility audit is repeated before commit. One Undo restores "
            "the exact Session v1 project. Existing expression and definition source "
            "text is retained; the new variable library starts empty, and the graph "
            "must be staged again.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Variables v1 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Variables v1 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Variables v1 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToVariablesV1();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToVariablesV1()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToVariablesV1();
    if (!result.ok())
    {
        setStatus(
            "Variables v1 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->showVariables();
    setStatus(
        "Upgraded project to Variables v1 as one Undo step | fresh staging required");
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToVariablesV2()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV1)
    {
        setStatus("Variables v2 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Variables v2 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Variables v2?")
        .withMessage(
            "Upgrade this project to format 36 and Variables v2?\n\n"
            "The compatibility audit is repeated before commit. One Undo restores "
            "the exact Variables v1 project. Existing expressions, definitions, "
            "and variable declarations are retained; the new checked rem: operator "
            "becomes available, and the graph must be staged again.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Variables v2 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Variables v2 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Variables v2 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToVariablesV2();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToVariablesV2()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToVariablesV2();
    if (!result.ok())
    {
        setStatus(
            "Variables v2 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        "Upgraded project to Variables v2 as one Undo step | fresh staging required");
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToVariablesV3()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        setStatus("Variables v3 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Variables v3 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Variables v3?")
        .withMessage(
            "Enable millisecond-clock ramp: expressions for this project?\n\n"
            "Existing expressions are retained. One Undo restores Variables v2. "
            "Timed parameter evaluation currently supports oscillator amplitude "
            "without Event Value or variable references. Restage the graph after upgrading.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Variables v3 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Variables v3 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Variables v3 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToVariablesV3();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToVariablesV3()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToVariablesV3();
    if (!result.ok())
    {
        setStatus(
            "Variables v3 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        "Upgraded project to Variables v3 as one Undo step | fresh staging required");
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToVariablesV4()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    const auto language = projectBridge_.editor().document().expressionLanguage;
    if (language != talas::expr::ExpressionLanguageVersion::VariablesV2
        && language != talas::expr::ExpressionLanguageVersion::VariablesV3)
    {
        setStatus("Variables v4 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Variables v4 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Variables v4?")
        .withMessage(
            "Enable units after expressions, such as !Interval nn and (!Length * 2) ms?\n\n"
            "Existing expression text is retained. One Undo restores the previous "
            "language. Restage the graph after upgrading.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Variables v4 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Variables v4 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Variables v4 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToVariablesV4();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToVariablesV4()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToVariablesV4();
    if (!result.ok())
    {
        setStatus(
            "Variables v4 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        "Upgraded project to Variables v4 as one Undo step | fresh staging required");
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToValV1()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV4)
    {
        setStatus("Val v1 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Val v1 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Val v1?")
        .withMessage(
            "Read parameter fields as Val, with messages such as 3 twoExp, "
            "4 c hz and 3 vmin: 2?\n\n"
            "Val applies binary operators from left to right; every existing "
            "expression was checked to keep its meaning. One Undo restores the previous "
            "language. Restage the graph after upgrading.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Val v1 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Val v1 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Val v1 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToValV1();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToValV1()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToValV1();
    if (!result.ok())
    {
        setStatus(
            "Val v1 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        "Upgraded project to Val v1 as one Undo step | fresh staging required");
}

void WorkbenchComponent::requestProjectExpressionsUpgradeToVoiceV1()
{
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus(
            "Finish the active dialog before upgrading project expressions");
        return;
    }
    if (projectBridge_.editor().document().expressionLanguage
        != talas::expr::ExpressionLanguageVersion::VariablesV2)
    {
        setStatus("Voice v1 upgrade is no longer available");
        return;
    }
    std::string expectedDocument;
    try
    {
        expectedDocument = currentDocumentSnapshot();
    }
    catch (const std::exception&)
    {
        setStatus(
            "Voice v1 upgrade unavailable: project snapshot allocation failed");
        return;
    }
    const auto expectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Upgrade Expressions to Voice v1?")
        .withMessage(
            "Enable ?VoiceNumber for this project?\n\n"
            "Each MIDI Voice slot gets its own number, starting at one. Use it "
            "to vary static seeds, frequencies and other voice parameters. "
            "Existing expressions are retained. One Undo restores Variables v2, "
            "and the instrument must be staged again.")
        .withButton("Upgrade")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis, expectedDocument, expectedAuthority](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Voice v1 upgrade cancelled");
                return;
            }
            try
            {
                if (safeThis->projectBridge_.editor()
                            .documentAuthorityGeneration()
                        != expectedAuthority
                    || safeThis->currentDocumentSnapshot()
                        != expectedDocument)
                {
                    safeThis->setStatus(
                        "Voice v1 upgrade rejected after project authority changed");
                    return;
                }
            }
            catch (const std::exception&)
            {
                safeThis->setStatus(
                    "Voice v1 upgrade rejected: project snapshot allocation failed");
                return;
            }
            safeThis->performProjectExpressionsUpgradeToVoiceV1();
        });
}

void WorkbenchComponent::performProjectExpressionsUpgradeToVoiceV1()
{
    const auto result =
        projectBridge_.editor().upgradeExpressionsToVoiceV1();
    if (!result.ok())
    {
        setStatus(
            "Voice v1 upgrade blocked: "
            + juceString(result.message));
        return;
    }
    documentChanged();
    inspectorPanel_->focusExpressionLanguage();
    setStatus(
        "Upgraded project to Voice v1 as one Undo step | fresh staging required");
}

void WorkbenchComponent::snapshotAuthoringChanged()
{
    fileOperationCoordinator_.advanceAuthority();
    invalidateAnalysisOutputTakeForProjectAuthorityChange();
    performanceCanvas_->refresh();
    undoButton_.setEnabled(projectBridge_.editor().canUndoIn(frontHistoryWindow()));
    redoButton_.setEnabled(projectBridge_.editor().canRedoIn(frontHistoryWindow()));
    commandManager_.commandStatusChanged();
    updateRecoveryAfterDocumentChange();
    refreshWindowTitle();
    refreshPersistentStatus();
}

void WorkbenchComponent::undoProject(std::optional<talas::project::ProjectEditor::HistoryWindow> window)
{
    suspendOscLoopbackProfile();
    const auto previousDevices =
        projectBridge_.editor().document().deviceSettings;
    const auto result = window.has_value() ? projectBridge_.editor().undoIn(*window) : projectBridge_.editor().undo();
    if (!result.ok())
    {
        rebuildOscEgress();
        setStatus(juceString(result.message));
        return;
    }

    const auto deviceWarning = talas::app::workbenchHistoryStepReopensDevices(
            previousDevices,
            projectBridge_.editor().document().deviceSettings)
        ? applyProjectDeviceSettings()
        : juce::String{};
    documentChanged();
    reconcileHierarchyAfterHistoryEdit();
    setStatus(
        deviceWarning.isEmpty()
            ? juce::String{"Undid project edit"}
            : juceString("Undid project edit · ") + deviceWarning);
}

void WorkbenchComponent::redoProject(std::optional<talas::project::ProjectEditor::HistoryWindow> window)
{
    suspendOscLoopbackProfile();
    const auto previousDevices =
        projectBridge_.editor().document().deviceSettings;
    const auto result = window.has_value() ? projectBridge_.editor().redoIn(*window) : projectBridge_.editor().redo();
    if (!result.ok())
    {
        rebuildOscEgress();
        setStatus(juceString(result.message));
        return;
    }

    const auto deviceWarning = talas::app::workbenchHistoryStepReopensDevices(
            previousDevices,
            projectBridge_.editor().document().deviceSettings)
        ? applyProjectDeviceSettings()
        : juce::String{};
    documentChanged();
    reconcileHierarchyAfterHistoryEdit();
    setStatus(
        deviceWarning.isEmpty()
            ? juce::String{"Redid project edit"}
            : juceString("Redid project edit · ") + deviceWarning);
}

void WorkbenchComponent::deleteSelectedNode()
{
    requestDeleteSelectedNodes(activeGraphContext_, selectedNodeIds_);
}

void WorkbenchComponent::duplicateSelectedNodes(
    std::vector<std::string> nodeIds)
{
    if (nodeIds.empty())
        nodeIds = selectedNodeIds_;
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        || nodeIds.empty())
    {
        setStatus("No active graph selection to duplicate");
        return;
    }
    std::vector<std::string> duplicated;
    const auto result = activeGraphContext_.has_value()
        ? projectBridge_.editor().duplicateNodesInSoundDefinition(
              *activeGraphContext_, nodeIds, duplicated)
        : projectBridge_.editor().duplicateNodes(
              nodeIds, duplicated);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    documentChanged();
    if (!duplicated.empty())
        publishGraphSelection(
            activeGraphContext_, duplicated, duplicated.front());
    setStatus(
        "Duplicated "
        + juce::String(static_cast<int>(duplicated.size()))
        + (duplicated.size() == 1 ? " node" : " nodes")
        + " as one project edit");
}

void WorkbenchComponent::selectAllGraphNodes()
{
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples)
    {
        setStatus("The active workspace does not expose a Structure graph");
        return;
    }
    const auto* graph = graphForContext(activeGraphContext_);
    if (graph == nullptr || graph->nodes.empty())
    {
        publishGraphSelection(activeGraphContext_, {}, {});
        setStatus("Active graph has no nodes to select");
        return;
    }
    std::vector<std::string> nodeIds;
    nodeIds.reserve(graph->nodes.size());
    for (const auto& node : graph->nodes)
        nodeIds.push_back(node.instanceId);
    publishGraphSelection(
        activeGraphContext_,
        std::move(nodeIds),
        graph->nodes.front().instanceId);
    setStatus(
        "Selected all "
        + juce::String(static_cast<int>(graph->nodes.size()))
        + " nodes in the active graph");
}

void WorkbenchComponent::focusPrototypeSearch()
{
    if (workspaceMode_ != talas::app::WorkbenchWorkspaceMode::Patch)
        activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    const auto policy = talas::app::workbenchLayoutPolicyForSize(
        getWidth(), getHeight());
    if (policy.useSummonableSideWorkspaces)
        compactDock_ = CompactDock::Library;
    else
        libraryVisible_ = true;
    resized();
    prototypePanel_->focusSearch();
    setStatus("Prototype search focused; press Return to add the selection");
}

void WorkbenchComponent::performGraphKeyboardCommand(
    GraphKeyboardCommandRequest request)
{
    const auto currentDocumentSnapshot =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        || request.definitionId != activeGraphContext_
        || graphForContext(request.definitionId) == nullptr
        || request.expectedDocumentSnapshot != currentDocumentSnapshot)
    {
        setStatus("Stale or hidden graph command rejected");
        return;
    }
    const auto selectionMatches = [&] {
        if (request.selectedNodeIds.size() != selectedNodeIds_.size())
            return false;
        std::unordered_set<std::string> selected(
            selectedNodeIds_.begin(), selectedNodeIds_.end());
        return selected.size() == selectedNodeIds_.size()
            && std::ranges::all_of(
                request.selectedNodeIds,
                [&](const auto& nodeId) {
                    return selected.contains(nodeId);
                });
    };
    switch (request.command)
    {
    case GraphKeyboardCommand::SelectAll:
        selectAllGraphNodes();
        return;
    case GraphKeyboardCommand::DuplicateSelection:
        if (!selectionMatches())
        {
            setStatus("Graph selection changed before duplicate command");
            return;
        }
        duplicateSelectedNodes(std::move(request.selectedNodeIds));
        return;
    case GraphKeyboardCommand::DeleteSelection:
        if (!selectionMatches())
        {
            setStatus("Graph selection changed before delete command");
            return;
        }
        requestDeleteSelectedNodes(
            request.definitionId,
            std::move(request.selectedNodeIds));
        return;
    case GraphKeyboardCommand::FocusPrototypeSearch:
        focusPrototypeSearch();
        return;
    case GraphKeyboardCommand::CopySelection:
        if (!selectionMatches())
        {
            setStatus("Graph selection changed before copy command");
            return;
        }
        (void)copySelectedNodes(std::move(request.selectedNodeIds));
        return;
    case GraphKeyboardCommand::CutSelection:
        if (!selectionMatches())
        {
            setStatus("Graph selection changed before cut command");
            return;
        }
        cutSelectedNodes(std::move(request.selectedNodeIds));
        return;
    case GraphKeyboardCommand::Paste:
        pasteGraphNodes(request.pastePosition);
        return;
    case GraphKeyboardCommand::RenameSelection:
        if (!selectionMatches() || request.selectedNodeIds.size() != 1)
        {
            setStatus("Select one Sound to rename");
            return;
        }
        promptRenameGraphNode(
            request.definitionId,
            std::move(request.selectedNodeIds.front()));
        return;
    case GraphKeyboardCommand::CleanUpLayout:
        (void)cleanUpGraphLayout(request.definitionId);
        return;
    case GraphKeyboardCommand::DescribeSelection:
    case GraphKeyboardCommand::InfoOnSelection:
        if (request.selectedNodeIds.size() == 1)
            describeGraphNode(request.definitionId, request.selectedNodeIds.front(),
                request.command == GraphKeyboardCommand::InfoOnSelection);
        return;
    case GraphKeyboardCommand::SetReplaceableInput:
        if (request.selectedNodeIds.size() == 1)
            setReplaceableInputOf(request.definitionId, request.selectedNodeIds.front());
        return;
    }
}

void WorkbenchComponent::requestDeleteSelectedNodes(
    std::optional<std::string> definitionId,
    std::vector<std::string> nodeIds,
    bool askFirst)
{
    if (workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Samples
        || definitionId != activeGraphContext_ || nodeIds.empty())
    {
        setStatus("No active graph selection to delete");
        return;
    }
    auto preservedCount = std::size_t{};
    for (const auto& nodeId : nodeIds)
    {
        const auto* node = nodeForContext(definitionId, nodeId);
        if (node == nullptr)
        {
            setStatus("Selected graph nodes are no longer exact");
            return;
        }
        if (node->preservedComponent.has_value())
            ++preservedCount;
    }
    if (preservedCount == 0)
    {
        // Asks before removing each Sound.
        if (askFirst)
            askToRemove({{}, nodeIds}, graphRemovalQuestion(definitionId, nodeIds.size() == 1),
                [this, definitionId](std::vector<std::string> removed) { performDeleteNodes(definitionId, removed); });
        else
            performDeleteNodes(definitionId, nodeIds);
        return;
    }
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus("Finish the active dialog before deleting selected nodes");
        return;
    }
    stopComponentReplacement();
    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto expectedDocument = currentDocumentSnapshot();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Delete Selected Nodes?")
        .withMessage(
            "Delete " + juce::String(static_cast<int>(nodeIds.size()))
            + " selected node"
            + (nodeIds.size() == 1 ? "" : "s")
            + ", including "
            + juce::String(static_cast<int>(preservedCount))
            + " preserved component"
            + (preservedCount == 1 ? "" : "s")
            + ", and all incident connections?\n\n"
              "One Undo restores the complete selected set.")
        .withButton("Delete")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis,
         definitionId = std::move(definitionId),
         nodeIds = std::move(nodeIds),
         expectedDocument](int choice) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (!talas::app::nativeConfirmationAccepted(choice))
            {
                safeThis->setStatus("Selected-node deletion cancelled");
                return;
            }
            if (safeThis->currentDocumentSnapshot() != expectedDocument
                || safeThis->activeGraphContext_ != definitionId)
            {
                safeThis->setStatus(
                    "Selected-node deletion rejected after graph authority changed");
                return;
            }
            safeThis->performDeleteNodes(definitionId, nodeIds);
        });
}

bool WorkbenchComponent::replacingAsks(
    std::optional<std::string_view> scope,
    std::string_view replacedNodeId,
    const talas::project::ProjectNode& replacement) const
{
    // When both have inputs.
    if (projectBridge_.editor().inputsReplacingNode(scope, replacedNodeId).empty())
        return false;
    const auto descriptor = talas::project::describeProjectNode(projectBridge_.editor().document(), replacement, registry_);
    return descriptor.has_value()
        && std::ranges::any_of(descriptor->inputs, [](const talas::core::PortDescriptor& input) {
               return !input.compilerPopulated && input.type == talas::core::PortType::Audio;
           });
}

bool WorkbenchComponent::evaluateSelectionIn(juce::TextEditor& editor, std::vector<talas::project::smalltalk::PromptAnswer> answers,
    std::vector<std::uint32_t> seeds)
{
    if (editor.isReadOnly())
    {
        setStatus("This text cannot be evaluated into");
        return false;
    }
    const auto text = editor.getText();
    const auto selected = editor.getHighlightedRegion();
    const auto bytesBefore = [&](int characters) {
        return text.substring(0, characters).toStdString().size();
    };
    auto sampleRate = 48000.0;
    if (auto* device = deviceManager_.getCurrentAudioDevice(); device != nullptr && device->getCurrentSampleRate() > 0.0)
        sampleRate = device->getCurrentSampleRate();
    talas::project::smalltalk::EvaluationHost host(sampleRate, projectBridge_.editor().document().expressionDefinitions.timecodeFramesPerSecond);
    host.answerFrom(answers, std::move(seeds));
    host.setStackTraces(preferences_.allowStackTraces);
    host.setRounding(projectBridge_.editor().document().expressionDefinitions.rounding);
    const auto source = text.toStdString();
    const auto evaluated = talas::project::smalltalk::evaluateSelection(
        source, bytesBefore(selected.getStart()), bytesBefore(selected.getEnd()), host);
    // A question for the user, asked before the
    // evaluation runs again with its answer.
    if (evaluated.pendingPrompt.has_value())
    {
        askEvaluationQuestion(editor, *evaluated.pendingPrompt, std::move(answers), host.seeds());
        return true;
    }
    showEvaluationDebugLines(evaluated.debugLines);
    showEvaluationGraphs(evaluated.graphs);
    if (!evaluated.ok())
    {
        // The field flashes and the error goes into the text
        // where it was found, selected.
        flashTextEditor(editor);
        const auto at = juce::String::fromUTF8(source.data(), static_cast<int>(std::min(
            evaluated.diagnostic->range.offset, bytesBefore(selected.getEnd())))).length();
        editor.setHighlightedRegion({at, at});
        editor.insertTextAtCaret(" " + juceString(evaluated.diagnostic->message));
        editor.setHighlightedRegion({at + 1, at + 1 + juceString(evaluated.diagnostic->message).length()});
        setStatus("Evaluate: " + juceString(evaluated.diagnostic->message));
        return false;
    }
    // The result goes in as typing would, so that Undo takes it back.
    const auto charactersOf = [&](std::size_t bytes) {
        return juce::String::fromUTF8(evaluated.text.data(), static_cast<int>(bytes)).length();
    };
    const auto at = charactersOf(evaluated.resultStart - 1);
    const auto printed = juce::String::fromUTF8(evaluated.text.data() + evaluated.resultStart,
        static_cast<int>(evaluated.resultEnd - evaluated.resultStart));
    editor.setHighlightedRegion({at, at});
    editor.insertTextAtCaret(" " + printed);
    editor.setHighlightedRegion({at + 1, at + 1 + printed.length()});
    setStatus("Evaluated: " + printed);
    return true;
}

bool WorkbenchComponent::EvaluateKeys::keyPressed(const juce::KeyPress& key, juce::Component* originating)
{
    auto* editor = dynamic_cast<juce::TextEditor*>(originating);
    if (editor == nullptr)
        return false;
    if (key == juce::KeyPress(juce::KeyPress::escapeKey) && editor->getComponentID() == "inspector.parameter.value")
    {
        owner.beginEventValueLearning(*editor);
        return true;
    }
    // Ctrl+Space plays from a field.
    if (const auto press = talas::app::parameterFieldTransportPress(key.getKeyCode() == ' ', key.getModifiers().isCtrlDown()))
    {
        owner.pressTransportKey(*press);
        return true;
    }
    if (key != juce::KeyPress('y', juce::ModifierKeys::commandModifier, 0))
        return false;
    owner.evaluateSelectionIn(*editor);
    return true;
}

void WorkbenchComponent::beginEventValueLearning(juce::TextEditor& field)
{
    eventValueLearning_ = {};
    learningField_ = &field;
    // The cursor turns into a little fader while the field waits.
    field.setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    midiLearnArmed_.store(true, std::memory_order_release);
    setStatus("Play one, two or three keys, or move a controller, to name its Event Value");
}

void WorkbenchComponent::learnFromMidi(const juce::MidiMessage& message)
{
    if (!midiLearnArmed_.load(std::memory_order_acquire))
        return;
    std::optional<std::string> name;
    if (message.isNoteOn())
        name = eventValueLearning_.key(true);
    else if (message.isNoteOff())
        name = eventValueLearning_.key(false, preferences_.learnOneKeyAsPitch);
    else if (message.isController())
    {
        name = talas::project::EventValueLearning::controller(projectBridge_.editor().document(),
            static_cast<std::uint8_t>(message.getControllerNumber()), static_cast<std::uint8_t>(message.getChannel()));
    }
    if (!name.has_value())
        return;
    midiLearnArmed_.store(false, std::memory_order_release);
    if (learningField_ == nullptr)
        return;
    learningField_->setMouseCursor(juce::MouseCursor::IBeamCursor);
    learningField_->insertTextAtCaret(juceString(*name));
    setStatus("Typed " + juceString(*name) + " from MIDI");
}

void WorkbenchComponent::performDeleteNodes(
    const std::optional<std::string>& definitionId,
    std::span<const std::string> nodeIds,
    std::optional<std::string> inputNodeId)
{
    const auto scope = optionalStringView(definitionId);
    // One Sound deleted gives its place to its input, asking which of
    // several; several go with their lines.
    if (nodeIds.size() == 1 && !inputNodeId.has_value())
    {
        auto inputs = projectBridge_.editor().inputsReplacingNode(scope, nodeIds.front());
        if (inputs.size() > 1)
        {
            chooseInput("Replace " + juceString(nodeIds.front()) + " with which input?", std::move(inputs),
                [this, definitionId, nodeId = nodeIds.front()](std::string input) {
                    const std::array selected{nodeId};
                    performDeleteNodes(definitionId, selected, std::move(input));
                });
            return;
        }
    }
    const auto result = nodeIds.size() == 1
        ? projectBridge_.editor().replaceNodeWithItsInput(scope, nodeIds.front(), optionalStringView(inputNodeId))
        : definitionId.has_value()
        ? projectBridge_.editor().removeNodesFromSoundDefinition(
              *definitionId, nodeIds)
        : projectBridge_.editor().removeNodes(nodeIds);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }
    if (definitionId == activeGraphContext_)
        selectNode(definitionId, {});
    documentChanged();
    setStatus(
        "Deleted " + juce::String(static_cast<int>(nodeIds.size()))
        + (nodeIds.size() == 1 ? " node" : " nodes")
        + " as one project edit");
}

void WorkbenchComponent::handlePreservedNodeAction(
    std::optional<std::string> definitionId,
    std::string nodeId,
    PreservedNodeAction action)
{
    switch (action)
    {
    case PreservedNodeAction::Retry:
    {
        const auto result = definitionId.has_value()
            ? projectBridge_.editor()
                  .repairPreservedComponentInSoundDefinition(
                      *definitionId,
                      nodeId)
            : projectBridge_.editor()
                  .repairPreservedComponent(nodeId);
        graphCanvas_->refreshModel();
        secondaryGraphCanvas_->refreshModel();
        inspectorPanel_->refresh();
        setStatus(
            result.ok()
                ? "Exact component repair is available"
                : juceString(result.message));
        return;
    }
    case PreservedNodeAction::Replace:
        beginComponentReplacement(
            std::move(definitionId),
            std::move(nodeId));
        return;
    case PreservedNodeAction::ConfirmReplacement:
        confirmComponentReplacement(definitionId, nodeId);
        return;
    case PreservedNodeAction::CancelReplacement:
        cancelComponentReplacement();
        return;
    case PreservedNodeAction::Disconnect:
    {
        const auto result = definitionId.has_value()
            ? projectBridge_.editor()
                  .disconnectPreservedComponentInSoundDefinition(
                      *definitionId,
                      nodeId)
            : projectBridge_.editor()
                  .disconnectPreservedComponent(nodeId);
        if (!result.ok())
        {
            setStatus(juceString(result.message));
            inspectorPanel_->refresh();
            return;
        }
        documentChanged();
        setStatus("Preserved node disconnected");
        return;
    }
    case PreservedNodeAction::Delete:
        requestDeleteNode(
            std::move(definitionId),
            std::move(nodeId));
        return;
    }
}

void WorkbenchComponent::requestDeleteNode(
    std::optional<std::string> definitionId,
    std::string nodeId)
{
    const auto* node = nodeForContext(definitionId, nodeId);
    if (node == nullptr)
    {
        setStatus("Selected node is no longer available");
        return;
    }
    if (!node->preservedComponent.has_value())
    {
        performDeleteNode(definitionId, nodeId);
        return;
    }
    if (destructivePromptActive_ || fileChooserActive_)
    {
        setStatus("Finish the active dialog before deleting this node");
        return;
    }
    stopComponentReplacement();

    destructivePromptActive_ = true;
    commandManager_.commandStatusChanged();
    const auto options = juce::MessageBoxOptions()
        .withIconType(juce::MessageBoxIconType::WarningIcon)
        .withTitle("Delete Preserved Component?")
        .withMessage(
            "Delete " + juceString(nodeId)
            + " and all of its existing connections?\n\n"
              "One Undo restores the complete preserved state.")
        .withButton("Delete")
        .withButton("Cancel")
        .withAssociatedComponent(this);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::NativeMessageBox::showAsync(
        options,
        [safeThis,
         definitionId = std::move(definitionId),
         nodeId = std::move(nodeId)](int result) {
            if (safeThis == nullptr)
                return;
            safeThis->destructivePromptActive_ = false;
            safeThis->scheduleWorkbenchFocusRestoration();
            safeThis->commandManager_.commandStatusChanged();
            if (talas::app::nativeConfirmationAccepted(result))
            {
                safeThis->performDeleteNode(
                    definitionId,
                    nodeId);
            }
            else
            {
                safeThis->setStatus(
                    "Preserved node deletion cancelled");
            }
        });
}

void WorkbenchComponent::performDeleteNode(
    const std::optional<std::string>& definitionId,
    std::string_view nodeId)
{
    const auto result = definitionId.has_value()
        ? projectBridge_.editor().removeNodeFromSoundDefinition(
              *definitionId,
              nodeId)
        : projectBridge_.editor().removeNode(nodeId);
    if (!result.ok())
    {
        setStatus(juceString(result.message));
        return;
    }

    if (definitionId == activeGraphContext_
        && selectedNodeId_ == nodeId)
    {
        selectNode(activeGraphContext_, {});
    }
    documentChanged();
    setStatus("Node deleted");
}

void WorkbenchComponent::showOverflowMenu()
{
    constexpr auto scale100 = 4100;
    constexpr auto scale115 = 4115;
    constexpr auto scale130 = 4130;
    constexpr auto scale150 = 4150;
    constexpr auto enhancedContrast = 4201;
    constexpr auto reduceMotion = 4202;
    juce::PopupMenu menu;
    menu.addCommandItem(&commandManager_, commandNewProject);
    menu.addCommandItem(&commandManager_, commandOpenProject);
    menu.addCommandItem(&commandManager_, commandSaveProjectAs);
    menu.addSeparator();
    menu.addCommandItem(&commandManager_, commandExportAudio);
    menu.addCommandItem(&commandManager_, commandPackageProject);
    menu.addCommandItem(&commandManager_, commandAudioDeviceSettings);
    menu.addSeparator();
    menu.addCommandItem(&commandManager_, commandUndo);
    menu.addCommandItem(&commandManager_, commandRedo);
    menu.addCommandItem(&commandManager_, commandCutSelectedNodes);
    menu.addCommandItem(&commandManager_, commandCopySelectedNodes);
    menu.addCommandItem(&commandManager_, commandPasteGraphNodes);
    menu.addCommandItem(&commandManager_, commandDeleteSelectedNode);
    menu.addCommandItem(&commandManager_, commandDuplicateSelectedNodes);
    menu.addCommandItem(&commandManager_, commandSelectAllGraphNodes);
    menu.addCommandItem(&commandManager_, commandFocusPrototypeSearch);
    menu.addSeparator();
    menu.addCommandItem(&commandManager_, commandWorkspacePatch);
    menu.addCommandItem(&commandManager_, commandWorkspaceAutomation);
    menu.addCommandItem(&commandManager_, commandWorkspaceAnalysis);
    menu.addCommandItem(&commandManager_, commandWorkspacePerformance);
    menu.addCommandItem(&commandManager_, commandWorkspaceSamples);
    menu.addSeparator();
    menu.addCommandItem(&commandManager_, commandToggleLibrary);
    menu.addCommandItem(&commandManager_, commandToggleRightWorkspace);
    menu.addCommandItem(&commandManager_, commandFocusGraph);
    menu.addCommandItem(&commandManager_, commandFocusTimeline);
    menu.addCommandItem(&commandManager_, commandZoomGraphToFit);
    menu.addSeparator();
    juce::PopupMenu accessibility;
    const auto currentScale = workspaceSession_.accessibility.interfaceScale;
    accessibility.addItem(
        scale100, "Interface scale 100%", true,
        currentScale == talas::project::WorkspaceInterfaceScale::Percent100);
    accessibility.addItem(
        scale115, "Interface scale 115%", true,
        currentScale == talas::project::WorkspaceInterfaceScale::Percent115);
    accessibility.addItem(
        scale130, "Interface scale 130%", true,
        currentScale == talas::project::WorkspaceInterfaceScale::Percent130);
    accessibility.addItem(
        scale150, "Interface scale 150%", true,
        currentScale == talas::project::WorkspaceInterfaceScale::Percent150);
    accessibility.addSeparator();
    accessibility.addItem(
        enhancedContrast, "Enhanced contrast", true,
        workspaceSession_.accessibility.enhancedContrast);
    accessibility.addItem(
        reduceMotion, "Reduce motion", true,
        workspaceSession_.accessibility.reduceMotion);
    menu.addSubMenu("Accessibility", accessibility);
    menu.addCommandItem(&commandManager_, commandToggleContextHelp);
    menu.addSeparator();
    menu.addCommandItem(&commandManager_, commandShowWorkbenchGuide);
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    menu.showMenuAsync(
        juce::PopupMenu::Options{}.withTargetComponent(moreButton_),
        [safeThis](int result) {
            if (safeThis == nullptr || result == 0)
                return;
            auto& preferences = safeThis->workspaceSession_.accessibility;
            switch (result)
            {
            case scale100:
                preferences.interfaceScale =
                    talas::project::WorkspaceInterfaceScale::Percent100;
                break;
            case scale115:
                preferences.interfaceScale =
                    talas::project::WorkspaceInterfaceScale::Percent115;
                break;
            case scale130:
                preferences.interfaceScale =
                    talas::project::WorkspaceInterfaceScale::Percent130;
                break;
            case scale150:
                preferences.interfaceScale =
                    talas::project::WorkspaceInterfaceScale::Percent150;
                break;
            case enhancedContrast:
                preferences.enhancedContrast =
                    !preferences.enhancedContrast;
                break;
            case reduceMotion:
                preferences.reduceMotion = !preferences.reduceMotion;
                break;
            default:
                return;
            }
            safeThis->applyAccessibilityPreferences();
            safeThis->noteWorkspaceSessionChanged();
            safeThis->setStatus("Accessibility preferences updated locally");
        });
}

void WorkbenchComponent::showWorkbenchGuide()
{
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::InfoIcon,
        "Workbench Guide",
        juceString("Workspaces\n"
        "Patch — discover prototypes, build the graph, inspect nodes, and operate Event Values.\n"
        "Automation — edit Timeline events beside the graph and Event Values.\n"
        "Analysis — measure the running Sound beside an expanded graph.\n"
        "Performance — operate the stable exposed controls of the selected "
        "reusable Sound instance.\n"
        "Samples — import referenced or embedded PCM16 WAV assets, map note "
        "and "
        "velocity regions, "
        "edit exact loop frames, and audition an isolated immutable "
        "generation. "
        "Type REPLACE before "
        "replacing every region in the active map.\n\n"
        "Graph navigation\n"
        "Scroll to pan. Command-scroll to zoom. Option-drag or middle-drag to pan. Command-0 fits the complete graph. Shift-click selects multiple nodes.\n\n"
        "Reusable Sounds\n"
        "Extract turns the selected root subgraph into one reusable Sound and substitutes typed boundary inputs. Inline restores an instance while applying its exposed overrides. Search by name or stable id; Bookmark, Restore View, and Recent preserve or revisit local hierarchy views. Double-click or Show in Structure opens a Sound opposite Root; double-clicking a nested Sound instance opens it opposite its parent. Breadcrumbs and Parent move back without losing each context's selection. Audition stages an isolated preview and keeps the active project graph unchanged if preparation fails.\n\n"
        "Snapshot performance\n"
        "Open Performance / Snapshots to create a project- or selected-node "
        "bank, store and recall snapshots, choose A/B morph endpoints, and use "
        "a "
        "reproducible decimal or 0x seed. Recall, morph, Randomize, and Random "
        "Merge are live actions. Store and library commands edit the project; "
        "Capture Current is the explicit one-undo writeback. Expand Parameter "
        "Policies to lock a target or supply the finite range required for "
        "numeric randomization. Stale targets stay visible with diagnostics "
        "and "
        "are never silently rebound.\n\n"
        "Asset recovery\n"
        "A missing, moved, inaccessible, or changed referenced asset opens "
        "Asset "
        "Recovery beside the graph. Locate File inspects one chosen file. "
        "Search "
        "Folder checks one selected local directory recursively for exact "
        "accepted SHA-256 matches within bounded limits. Apply Relocations "
        "commits all exact matches as one Undo step. Different bytes require "
        "Replace, review of the old and new hashes, and Confirm Replace before "
        "the generation advances. Ignore and Cancel leave the project "
        "unchanged; "
        "embedded package-integrity failures never use external recovery.\n\n"
        "Explicit feedback\n"
        "Give one FeedbackLoopInput and one FeedbackLoopOutput the same "
        "non-empty connection name inside a reusable Sound. The Output delay "
        "is "
        "a whole-frame prepared delay with a one-sample minimum; ordinary "
        "algebraic graph cycles remain invalid. Open "
        "examples/reusable_feedback_help.talas.json for a runnable local "
        "example.\n\n"
        "M3 runtime examples\n"
        "Open examples/README.md for five local format-10 projects covering "
        "native and historical FFT paths, immutable DiskPlayer/DiskCache "
        "health, "
        "Matrix4 to Output4 routing, and LiveSpectralAnalysis partial frames. "
        "Validate them with --status at 8192 Hz, or open a project here and "
        "select its FFT, stream, matrix, output, analysis, or legacy-hold node "
        "to inspect the same prepared contract.\n\n"
        "Signal colors\n"
        "Audio — teal · Control — amber · Event — magenta · Data — periwinkle "
        "· "
        "Output — blue. Port labels prefix the signal type with A, C, E, or D; "
        "assistive descriptions state the full type.\n\n"
        "Prototype maturity\n"
        "FULL — no documented subset restriction · SUBSET — functional with "
        "documented limits · PLACEHOLDER — catalog presence without full "
        "runtime "
        "behavior · UTILITY — native Talas utility.\n\n"
        "Analyzer cursor\n"
        "Use Left and Right Arrow; hold Shift for fine movement. Home and End jump to the plot edges. Escape clears the cursor."),
        "Close",
        this);
}

}
