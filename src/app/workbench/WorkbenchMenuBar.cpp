#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TalasKeyEquivalents.h"

namespace talas::app::workbench
{
// The menu bar: File, Edit, DSP,
// Action, Info and Tools, then View and Help.
juce::StringArray WorkbenchComponent::getMenuBarNames()
{
    juce::StringArray names;
    for (std::size_t index = 0; index < talas::app::talasMenuCount; ++index)
        names.add(juceString(talas::app::talasMenuName(static_cast<talas::app::TalasMenu>(index))));
    return names;
}

juce::PopupMenu WorkbenchComponent::getMenuForIndex(
    int topLevelMenuIndex,
    const juce::String&)
{
    juce::PopupMenu menu;
    const auto add = [this, &menu](juce::CommandID command) { menu.addCommandItem(&commandManager_, command); };
    if (topLevelMenuIndex < 0 || static_cast<std::size_t>(topLevelMenuIndex) >= talas::app::talasMenuCount)
        return menu;
    switch (static_cast<talas::app::TalasMenu>(topLevelMenuIndex))
    {
    case talas::app::TalasMenu::File:
    {
        // The File menu's Status, the memory with the DSP's.
        for (const auto command : {commandNewProject, commandNewFile, commandOpenProject, commandShowFileOrganizer,
                 commandShowSoundBrowser, commandShowSoundFileWindow, commandShowVirtualControlSurface, commandShowDspStatus,
                 commandShowSystemPrototypes,
                 commandOpenCustomPrototypes, commandNewTool, commandOpenTool, commandUseTool, commandOpenAny,
                 commandPlayFile, commandCloseWindow, commandChooseGlobalMap, commandChooseWindow})
            add(command);
        juce::PopupMenu recentMenu;
        const auto paths = recentProjects_.entries();
        for (std::size_t index = 0; index < paths.size(); ++index)
        {
            auto label = jucePathString(paths[index].filename());
            if (!paths[index].parent_path().empty())
                label += juceString("  —  ") + jucePathString(paths[index].parent_path());
            recentMenu.addItem(recentProjectMenuItemBase + static_cast<int>(index), label);
        }
        menu.addSubMenu("Open Recent", recentMenu, !paths.empty());
        menu.addSubMenu("Documentation", documentationMenu());
        menu.addSeparator();
        add(commandSaveProject);
        add(commandSaveProjectAs);
        menu.addSeparator();
        add(commandExportAudio);
        add(commandPackageProject);
        break;
    }
    case talas::app::TalasMenu::Edit:
        for (const auto command : {commandUndo, commandRedo, commandLargeWindow, commandZoomIn, commandZoomOut})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandCutSelectedNodes, commandCopySelectedNodes, commandPasteGraphNodes,
                 commandDuplicateSelectedNodes, commandDeleteSelectedNode})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandSelectAllGraphNodes, commandRenameGraphNode, commandCleanUpGraph})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandPasteSpecial, commandShowPreferences, commandPasteHot, commandView,
                 commandFindText, commandFindTextAgain, commandReplaceText, commandReplaceTextAgain})
            add(command);
        menu.addSeparator();
        add(commandCaptureDiskCacheLoop);
        appendFieldReadingMenu(menu);
        break;
    // Configuring and monitoring the signal processor.
    case talas::app::TalasMenu::Dsp:
        for (const auto command : {commandShowDspStatus, commandStop, commandRestart, commandMidiNotesOff,
                 commandShowMidiMessages, commandConfigureMidi, commandInitializeDsp, commandAudioDeviceSettings})
            add(command);
        break;
    // Acting on the selected Sounds.
    case talas::app::TalasMenu::Action:
        for (const auto command : {commandPlay, commandPlayMarked, commandPlayAll, commandPause, commandCompileAndLoad,
                 commandCompileToDisk, commandRecordToDisk})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandExpandSound, commandRevertSound, commandFocusPrototypeSearch, commandEvaluateSelection,
                 commandNewClassFromExample, commandEditClass, commandRetrieveExampleFromClass, commandSetDefaultSound,
                 commandSetDefaultCollection})
            add(command);
        break;
    // Information on the selected Sounds.
    case talas::app::TalasMenu::Info:
        for (const auto command : {commandDescribeSound, commandGetInfo, commandShowStructureAsText, commandShowFullWaveform,
                 commandInfoOscilloscope, commandInfoSpectrumAnalyzer})
            add(command);
        menu.addSeparator();
        add(commandShowEnvironment);
        add(commandResetEnvironment);
        break;
    case talas::app::TalasMenu::Tools:
        for (const auto command : {commandShowTapeRecorder, commandShowFakeKeyboard, commandShowLiveGridEditor,
                 commandShowExpressionsLibrary, commandShowSampleEditor, commandShowGlobalMap, commandImportMidiToTimeline,
                 commandShowClassEditor, commandShowAlternateTunings, commandFileArchivist})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandShowSpectralAnalysis, commandShowReAnalysis, commandShowSynchronizingSpectra,
                 commandShowGaAnalysis, commandShowCdpProcess})
            add(command);
        appendSavedToolsMenu(menu);
        break;
    case talas::app::TalasMenu::View:
    {
        for (const auto command : {commandWorkspacePatch, commandWorkspaceAutomation, commandWorkspaceAnalysis,
                 commandWorkspacePerformance, commandWorkspaceSamples})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandToggleLibrary, commandToggleRightWorkspace, commandToggleContextHelp})
            add(command);
        menu.addSeparator();
        for (const auto command : {commandFocusGraph, commandFocusTimeline, commandZoomGraphToFit})
            add(command);
        menu.addSeparator();
        juce::PopupMenu structureLayoutMenu;
        for (const auto command : {commandStructureSplitAdaptive, commandStructureSplitLeftRight, commandStructureSplitTopBottom})
            structureLayoutMenu.addCommandItem(&commandManager_, command);
        structureLayoutMenu.addSeparator();
        structureLayoutMenu.addCommandItem(&commandManager_, commandSwapStructurePanes);
        structureLayoutMenu.addCommandItem(&commandManager_, commandToggleSecondaryStructurePane);
        menu.addSubMenu("Structure Layout", structureLayoutMenu);
        juce::PopupMenu graphPresentationMenu;
        for (const auto command : {commandGraphViewCompact, commandGraphViewPorts, commandGraphViewPortsAndWires})
            graphPresentationMenu.addCommandItem(&commandManager_, command);
        menu.addSubMenu("Graph Presentation", graphPresentationMenu);
        break;
    }
    case talas::app::TalasMenu::Help:
        add(commandShowWorkbenchGuide);
        break;
    }
    return menu;
}
}
