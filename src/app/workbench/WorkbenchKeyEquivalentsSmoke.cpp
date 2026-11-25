#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/TalasKeyEquivalents.h"

namespace talas::app::workbench
{
// The
// transport's keys; Ctrl+K stops,
// Ctrl+L opens the large window, and Ctrl+] and Ctrl+[ zoom the signal flow
// in and out.
[[nodiscard]] std::optional<std::string> WorkbenchComponent::runKeyEquivalentsSmoke()
{
    auto* mappings = commandManager_.getKeyMappings();
    const auto mapped = [&](juce::CommandID command, talas::app::TalasKeyCommand key) {
        return mappings != nullptr
            && mappings->containsMapping(command,
                juce::KeyPress{talas::app::talasKeyEquivalent(key).key, juce::ModifierKeys::commandModifier, 0});
    };
    if (!mapped(commandStop, talas::app::TalasKeyCommand::Stop) || !mapped(commandLargeWindow, talas::app::TalasKeyCommand::LargeWindow)
        || !mapped(commandZoomIn, talas::app::TalasKeyCommand::ZoomIn) || !mapped(commandZoomOut, talas::app::TalasKeyCommand::ZoomOut)
        || !mapped(commandFocusPrototypeSearch, talas::app::TalasKeyCommand::FindPrototype)
        || !mapped(commandFindText, talas::app::TalasKeyCommand::Find) || !mapped(commandFindTextAgain, talas::app::TalasKeyCommand::FindAgain)
        || !mapped(commandReplaceText, talas::app::TalasKeyCommand::Replace)
        || !mapped(commandExpandSound, talas::app::TalasKeyCommand::Expand)
        || !mapped(commandMidiNotesOff, talas::app::TalasKeyCommand::MidiNotesOff)
        || !mapped(commandCloseWindow, talas::app::TalasKeyCommand::Close)
        || !mappings->containsMapping(commandNewFile, juce::KeyPress{'n', juce::ModifierKeys::commandModifier, 0}))
        return "Ctrl+K, Ctrl+L, Ctrl+], Ctrl+[, Ctrl+B, Ctrl+F, Ctrl+G, Ctrl+J, Ctrl+E, Ctrl+M, Ctrl+W and Ctrl+N were not the key equivalents of their commands";
    // A menu item lists its key equivalent at its
    // right, usually its name's first letter: Ctrl+S for Save, Ctrl+O for
    // Open....
    auto listed = 0;
    const auto fileMenu = getMenuForIndex(getMenuBarNames().indexOf("File"), "File");
    for (juce::PopupMenu::MenuItemIterator item{fileMenu}; item.next();)
    {
        const auto& entry = item.getItem();
        listed += (entry.itemID == commandSaveProject || entry.itemID == commandOpenProject) && entry.commandManager != nullptr
            && !entry.commandManager->getKeyMappings()->getKeyPressesAssignedToCommand(entry.itemID).isEmpty();
    }
    if (listed != 2 || !mappings->containsMapping(commandSaveProject, juce::KeyPress{'s', juce::ModifierKeys::commandModifier, 0})
        || !mappings->containsMapping(commandOpenProject, juce::KeyPress{'o', juce::ModifierKeys::commandModifier, 0}))
        return "File > Save and Open... did not list Ctrl+S and Ctrl+O at their right";
    // Space plays and pauses, Ctrl+Space
    // rewinds and plays every Sound, Shift+Space the marked ones, and
    // Ctrl+Right Arrow plays.
    if (mappings == nullptr || !mappings->containsMapping(commandTogglePlayback, juce::KeyPress{' '})
        || !mappings->containsMapping(commandPlayAll, juce::KeyPress{' ', juce::ModifierKeys::ctrlModifier, 0})
        || !mappings->containsMapping(commandPlayMarked, juce::KeyPress{' ', juce::ModifierKeys::shiftModifier, 0})
        || !mappings->containsMapping(commandPlay,
            juce::KeyPress{juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0}))
        return "Space, Ctrl+Space, Shift+Space and Ctrl+Right Arrow were not the transport's keys";
    // The transport's state, as Space and Ctrl+R
    // read it: after an edit, then stopped.
    const auto wasStopped = transportStopped_;
    transportStopped_ = false;
    documentChanged();
    const auto edited = talasTransportState();
    stopCurrentProject();
    const auto stopped = talasTransportState();
    transportStopped_ = wasStopped;
    // File > Virtual Control Surface brings
    // the surface's workspace back.
    const auto previousMode = workspaceMode_;
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    const auto surfaceShown = commandManager_.invokeDirectly(commandShowVirtualControlSurface, false)
        && workspaceMode_ == talas::app::WorkbenchWorkspaceMode::Patch;
    activateWorkspaceMode(previousMode);
    if (!surfaceShown)
        return "File > Virtual Control Surface did not bring the surface's workspace back";
    if (edited != talas::app::TalasTransportState::Edited || stopped != talas::app::TalasTransportState::Stopped)
        return "the transport's state after an edit and after Stop was not read as Space and Ctrl+R need it";
    // Edit > View... sets the Sound editor's
    // connections, a Sound file window's icons and the organizer's folders.
    using talas::app::ViewedWindow;
    if (!applyViewChoice(ViewedWindow::SoundEditor, "Right angle")
        || graphCanvas_->lineContour() != talas::app::GraphLineContour::RightAngle
        || !viewChoiceTicked(ViewedWindow::SoundEditor, "Right angle") || !applyViewChoice(ViewedWindow::SoundEditor, "Curved"))
        return "Edit > View... did not set the Sound editor's connections";
    const auto scaleBefore = graphCanvas_->viewScaleForSmoke();
    const auto smallIcons = applyViewChoice(ViewedWindow::SoundEditor, "Small icons")
        && std::abs(graphCanvas_->viewScaleForSmoke() - 0.5F) < 1.0e-3F;
    graphCanvas_->setIconScale(scaleBefore);
    if (!smallIcons)
        return "Edit > View... did not show the signal flow's small icons at half the size";
    // Prototypes across the top puts the
    // prototypes in a strip above the Sound definitions.
    const auto strip = applyViewChoice(ViewedWindow::SoundEditor, "Prototypes across the top")
        && viewChoiceTicked(ViewedWindow::SoundEditor, "Prototypes across the top")
        && (!prototypePanel_->isVisible()
            || (prototypePanel_->getWidth() > 2 * prototypePanel_->getHeight()
                && prototypePanel_->getBottom() <= soundDefinitionsPanel_->getY()));
    const auto column = applyViewChoice(ViewedWindow::SoundEditor, "Prototypes at the left")
        && (!prototypePanel_->isVisible()
            || (prototypePanel_->getBottom() <= soundDefinitionsPanel_->getY()
                && prototypePanel_->getX() == soundDefinitionsPanel_->getX()
                && prototypePanel_->getWidth() == soundDefinitionsPanel_->getWidth()));
    if (!strip || !column)
        return "Edit > View... did not put the prototypes across the top and back at the left";
    showSoundFileWindow();
    showFileOrganizer();
    if (!applyViewChoice(ViewedWindow::SoundFileWindow, "Small icons") || soundFilePanel_->largeIcons()
        || !applyViewChoice(ViewedWindow::SoundFileWindow, "Large icons")
        || !applyViewChoice(ViewedWindow::FileOrganizer, "Folders among the files") || fileOrganizerPanel_->foldersFirst()
        || !applyViewChoice(ViewedWindow::FileOrganizer, "Folders first"))
        return "Edit > View... did not set a Sound file window's icons or the organizer's folders";
    const auto names = getMenuBarNames();
    const auto editMenu = getMenuForIndex(names.indexOf("Edit"), "Edit");
    auto zoomListed = false;
    for (juce::PopupMenu::MenuItemIterator item{editMenu}; item.next();)
        zoomListed = zoomListed || item.getItem().itemID == commandZoomIn;
    if (!zoomListed)
        return "Zoom In was not in the Edit menu";
    // The menus come in order, and each command in the
    // DSP, Action and Info menus belongs to the menu it is in.
    if (names.size() < 6 || names[0] != "File" || names[1] != "Edit" || names[2] != "DSP" || names[3] != "Action"
        || names[4] != "Info" || names[5] != "Tools")
        return "the menu bar was not File, Edit, DSP, Action, Info and Tools";
    for (const auto* menuName : {"DSP", "Action", "Info"})
    {
        const auto menu = getMenuForIndex(names.indexOf(menuName), menuName);
        for (juce::PopupMenu::MenuItemIterator item{menu}; item.next();)
        {
            const auto* command = commandManager_.getCommandForID(item.getItem().itemID);
            if (item.getItem().itemID != 0 && (command == nullptr || command->categoryName != menuName))
                return "the " + std::string{menuName} + " menu held " + item.getItem().text.toStdString() + " of another menu";
        }
    }
    graphCanvas_->grabKeyboardFocus();
    const auto before = graphCanvas_->viewScaleForSmoke();
    if (!commandManager_.invokeDirectly(commandZoomIn, false) || !(graphCanvas_->viewScaleForSmoke() > before))
        return "Zoom In did not magnify the signal flow";
    if (!commandManager_.invokeDirectly(commandZoomOut, false)
        || std::abs(graphCanvas_->viewScaleForSmoke() - before) > 1.0e-4F)
        return "Zoom Out did not take the signal flow back";
    // Show MIDI Messages lists what arrives.
    if (!commandManager_.invokeDirectly(commandShowMidiMessages, false) || midiMessagesText_ == nullptr
        || !midiMessagesText_->isReadOnly())
        return "DSP > MIDI Messages did not open its window";
    const std::array<std::uint8_t, 3> key{0x90, 60, 100};
    appendMidiMessageLine(key);
    midiMessagesShown_.store(false, std::memory_order_release);
    if (!midiMessagesText_->getText().contains("Key down 60 velocity 100 channel 1"))
        return "MIDI Messages did not show the key that arrived: " + midiMessagesText_->getText().toStdString();
    // Configure MIDI's Default Input Channel is kept and
    // given to the Sounds as they compile.
    juce::ApplicationCommandInfo configure(commandConfigureMidi);
    getCommandInfo(commandConfigureMidi, configure);
    const auto channelBefore = preferences_.defaultMidiInputChannel;
    setDefaultMidiInputChannel(3);
    const auto chosen = preferences_.defaultMidiInputChannel;
    setDefaultMidiInputChannel(channelBefore);
    if (configure.shortName != "Configure MIDI..." || chosen != 3 || preferences_.defaultMidiInputChannel != channelBefore)
        return "DSP > Configure MIDI did not keep the Default Input Channel";
    // DSP > Initialize DSP opens the audio device again.
    juce::ApplicationCommandInfo initialize(commandInitializeDsp);
    getCommandInfo(commandInitializeDsp, initialize);
    if (initialize.shortName != "Initialize DSP" || !commandManager_.invokeDirectly(commandInitializeDsp, false)
        || !statusLabel_.getText().contains("DSP"))
        return "DSP > Initialize DSP did not answer: " + statusLabel_.getText().toStdString();
    return std::nullopt;
}
}
