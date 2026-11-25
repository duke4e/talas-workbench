#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/app/SoundFileWindowModel.h"

#include <algorithm>

// Several Sound editors: any number open at once,
// one on each Sound, the one in front shown in the Sound editor pane and the
// others as tabs across its top; and Undo by the window in front.
namespace talas::app::workbench
{
namespace
{
constexpr int tabHeight = 22;
constexpr int closeBoxSize = 14;
constexpr std::string_view soundFileWindowName = "Sound File";
}

SoundEditorTabs::SoundEditorTabs()
{
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
    setTitle("Sound editors");
    setDescription("The Sound editors open, one on each Sound: a click brings one to the front, its x closes it");
}

void SoundEditorTabs::setTabs(std::vector<Tab> tabs)
{
    if (tabs == tabs_)
        return;
    tabs_ = std::move(tabs);
    repaint();
}

int SoundEditorTabs::tabWidth(std::size_t index) const
{
    const auto text = juce::GlyphArrangement::getStringWidthInt(WorkbenchType::microStrong(), tabs_[index].title);
    return std::clamp(text + 16 + closeBoxSize + 6, 64, 180);
}

int SoundEditorTabs::preferredWidth() const
{
    auto width = 0;
    for (std::size_t index = 0; index < tabs_.size(); ++index)
        width += tabWidth(index) + 2;
    return width;
}

juce::Rectangle<int> SoundEditorTabs::tabBounds(std::size_t index) const
{
    auto x = 0;
    for (std::size_t before = 0; before < index; ++before)
        x += tabWidth(before) + 2;
    return {x, 0, tabWidth(index), std::min(getHeight(), tabHeight)};
}

juce::Rectangle<int> SoundEditorTabs::closeBounds(std::size_t index) const
{
    const auto tab = tabBounds(index);
    return juce::Rectangle<int>{closeBoxSize, closeBoxSize}.withCentre({tab.getRight() - 4 - closeBoxSize / 2, tab.getCentreY()});
}

void SoundEditorTabs::paint(juce::Graphics& graphics)
{
    for (std::size_t index = 0; index < tabs_.size(); ++index)
    {
        const auto& tab = tabs_[index];
        const auto bounds = tabBounds(index);
        if (bounds.getX() >= getWidth())
            break;
        WorkbenchStyle::paintTile(graphics, bounds.toFloat(), 5.0F,
            tab.front ? workbenchTheme.surfaceRaised.brighter(0.1F) : panelColour.withAlpha(0.94F), tab.front);
        if (tab.front)
        {
            // The front tab outlined in the signature gradient.
            graphics.setGradientFill(WorkbenchStyle::accentGradient(bounds.toFloat()));
            graphics.drawRoundedRectangle(bounds.toFloat().reduced(0.75F), 5.0F, 1.5F);
        }
        graphics.setColour(tab.front ? textColour : secondaryTextColour);
        graphics.setFont(tab.front ? WorkbenchType::microStrong() : WorkbenchType::micro());
        graphics.drawFittedText(tab.title, bounds.withTrimmedLeft(8).withTrimmedRight(closeBoxSize + 8),
            juce::Justification::centredLeft, 1);
        const auto close = closeBounds(index).toFloat().reduced(4.0F);
        graphics.setColour(secondaryTextColour);
        graphics.drawLine({close.getTopLeft(), close.getBottomRight()}, 1.2F);
        graphics.drawLine({close.getTopRight(), close.getBottomLeft()}, 1.2F);
    }
}

void SoundEditorTabs::clickTab(std::size_t index, bool onCloseBox)
{
    if (index >= tabs_.size())
        return;
    const auto definitionId = tabs_[index].definitionId;
    if (onCloseBox)
    {
        if (onClose)
            onClose(definitionId);
    }
    else if (onSelect)
    {
        onSelect(definitionId);
    }
}

void SoundEditorTabs::mouseUp(const juce::MouseEvent& event)
{
    if (event.mouseWasDraggedSinceMouseDown())
        return;
    for (std::size_t index = 0; index < tabs_.size(); ++index)
    {
        if (tabBounds(index).contains(event.getPosition()))
        {
            clickTab(index, closeBounds(index).expanded(2).contains(event.getPosition()));
            return;
        }
    }
}

void WorkbenchComponent::syncSoundEditors()
{
    const auto& document = projectBridge_.editor().document();
    // Another Sound file opened: the editors on the old one close.
    if (const auto replacements = projectBridge_.editor().documentReplacements(); replacements != soundEditorsReplacement_)
    {
        soundEditorsReplacement_ = replacements;
        talas::app::soundEditorsOnNewDocument(soundEditors_, document);
    }
    talas::app::reconcileSoundEditors(soundEditors_, document);
    talas::app::followSoundEditorPath(soundEditors_, document, hierarchyPath_);
}

void WorkbenchComponent::refreshSoundEditors()
{
    syncSoundEditors();
    const auto& document = projectBridge_.editor().document();
    std::vector<SoundEditorTabs::Tab> tabs;
    tabs.reserve(soundEditors_.editors.size());
    for (const auto& editor : soundEditors_.editors)
        tabs.push_back({editor.definitionId, juceString(talas::app::soundEditorTitle(document, editor)),
            soundEditors_.front == editor.definitionId});
    soundEditorTabs_.setTabs(std::move(tabs));
    layoutSoundEditorTabs();
    updateHistoryWindow();
}

void WorkbenchComponent::layoutSoundEditorTabs()
{
    // Across the top of the pane the Sound editor shows in, right of its
    // badge; shown while there is an editor behind to bring forward.
    auto* pane = secondaryGraphCanvas_->isVisible() && !secondaryGraphCanvas_->getBounds().isEmpty()
        ? secondaryGraphCanvas_.get()
        : graphCanvas_.get();
    const auto& editors = soundEditors_.editors;
    const auto behind = editors.size() > 1 || (editors.size() == 1 && !soundEditors_.front.has_value());
    const auto left = pane->getX() + 196;
    const auto width = std::min(soundEditorTabs_.preferredWidth(), pane->getRight() - 8 - left);
    const auto shown = behind && pane->isVisible() && width >= 64;
    soundEditorTabs_.setVisible(shown);
    if (!shown)
        return;
    soundEditorTabs_.setBounds(left, pane->getY() + 5, width, tabHeight);
    soundEditorTabs_.toFront(false);
}

// The editor of a Sound being edited to the front,
// where it had gone, its selection and open parameters as they were.
void WorkbenchComponent::bringSoundEditorToFront(const std::string& definitionId)
{
    syncSoundEditors();
    const auto* editor = talas::app::soundEditorOn(soundEditors_, definitionId);
    if (editor == nullptr)
    {
        showSoundDefinition(definitionId);
        return;
    }
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    // The editor in front takes the Action and Info menus again, its
    // signal flow and selection as they were (no reopening).
    if (hierarchyPath_ != editor->path || activeGraphContext_ != secondaryGraphContext_ || !secondaryGraphContext_.has_value())
    {
        hierarchyPath_ = editor->path;
        secondaryGraphContext_ = hierarchyPath_.back();
        primaryGraphContext_ = hierarchyPath_.size() > 1
            ? std::optional<std::string>{hierarchyPath_[hierarchyPath_.size() - 2]}
            : std::optional<std::string>{};
        workspaceSession_.secondaryPaneVisible = true;
        activeGraphContext_ = secondaryGraphContext_;
        applyHierarchyContexts();
    }
    if (auto* window = getTopLevelComponent(); window != nullptr && isShowing())
        window->toFront(true);
    if (secondaryGraphCanvas_->isShowing())
        secondaryGraphCanvas_->grabKeyboardFocus();
    frontWindowName_.clear();
    updateHistoryWindow();
    setStatus("The Sound editor on " + juceString(definitionId) + " is in front, as it was");
}

void WorkbenchComponent::closeSoundEditorTab(const std::string& definitionId)
{
    syncSoundEditors();
    const auto* editor = talas::app::soundEditorOn(soundEditors_, definitionId);
    if (editor == nullptr)
        return;
    if (soundEditors_.front == definitionId)
    {
        closeSoundEditor();
        return;
    }
    if (editor->opened.has_value() && talas::app::soundEditorChanged(*editor->opened, projectBridge_.editor().document()))
    {
        // Its Sound changed: in front, to ask Keep, Drop or Cancel.
        bringSoundEditorToFront(definitionId);
        closeSoundEditor();
        return;
    }
    talas::app::closeSoundEditor(soundEditors_, definitionId);
    refreshSoundEditors();
    if (soundFilePanel_ != nullptr)
        soundFilePanel_->refresh();
    setStatus("Closed the Sound editor on " + juceString(definitionId));
}

void WorkbenchComponent::noteFrontWindow(juce::Component* focused)
{
    // Pop-up menus and dialogs leave the window in front as it was.
    if (focused == nullptr)
        return;
    auto* top = focused->getTopLevelComponent();
    if (top == nullptr || !top->isOnDesktop())
        return;
    if (top == getTopLevelComponent())
        frontWindowName_.clear();
    else if (auto* window = dynamic_cast<juce::DocumentWindow*>(top); window != nullptr && !window->isCurrentlyModal())
        frontWindowName_ = window->getName().toStdString();
    else
        return;
    updateHistoryWindow();
}

talas::project::ProjectEditor::HistoryWindow WorkbenchComponent::frontHistoryWindow() const
{
    return frontWindowName_.empty() ? talas::app::soundEditorHistoryWindow() : talas::app::namedHistoryWindow(frontWindowName_);
}

void WorkbenchComponent::updateHistoryWindow()
{
    auto& editor = projectBridge_.editor();
    const auto window = frontHistoryWindow();
    if (editor.historyWindow() == window)
        return;
    editor.setHistoryWindow(window);
    undoButton_.setEnabled(editor.canUndoIn(window));
    redoButton_.setEnabled(editor.canRedoIn(window));
    commandManager_.commandStatusChanged();
}

bool WorkbenchComponent::renameSoundFileVariable(const std::string& definitionId, const std::string& name)
{
    const auto& document = projectBridge_.editor().document();
    if (talas::app::soundFileVariable(document, definitionId) == nullptr)
        return false;
    auto renamed = talas::app::soundFileVariableRenamed(document, definitionId, name);
    if (!renamed.has_value())
    {
        setStatus("A variable's name is a letter, then letters and digits, with no spaces");
        return true;
    }
    if (const auto replaced = projectBridge_.editor().replaceSoundDefinition(definitionId, std::move(*renamed)); !replaced.ok())
    {
        setStatus(juceString(replaced.message));
        return true;
    }
    documentChanged();
    setStatus("Renamed the variable " + juceString(name) + "; drag it into an Inputs field to use it under that name");
    return true;
}

// New class from example applies there.
bool WorkbenchComponent::soundFileWindowInFront() const
{
    return soundFilePanel_ != nullptr && frontWindowName_ == soundFileWindowName
        && (soundFileWindow_ == nullptr || soundFileWindow_->isVisible());
}
}
