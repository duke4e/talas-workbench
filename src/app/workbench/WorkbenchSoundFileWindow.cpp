#include "app/workbench/WorkbenchSoundFileWindow.h"

#include "app/workbench/WorkbenchSoundIconPainting.h"
#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/SoundClassIcon.h"
#include "talas/app/SoundIcons.h"

#include <algorithm>
#include <utility>

// The Sound file window: an icon for each of the project's Sounds and Sound
// collections at a level, its name below; a collection opens to its own.
namespace talas::app::workbench
{
namespace
{
constexpr int margin = 12;
constexpr int header = 22;

class SoundFilePanelImpl final : public SoundFilePanel, public juce::DragAndDropContainer, public juce::DragAndDropTarget
{
public:
    explicit SoundFilePanelImpl(SoundFileHost host)
        : host_(std::move(host))
    {
        setWantsKeyboardFocus(true);
        setSize(560, 360);
        refresh();
    }

    void refresh() override
    {
        // A collection deleted while open shows the top level.
        if (level_.has_value() && !host_.collectionName(*level_).has_value())
            level_.reset();
        levelName_ = level_.has_value() ? host_.collectionName(*level_).value_or(std::string{}) : std::string{};
        icons_ = host_.icons(level_);
        std::erase_if(selection_, [&](const std::string& id) {
            return std::ranges::none_of(icons_, [&](const talas::app::SoundFileIcon& icon) { return icon.definitionId == id; });
        });
        descriptors_.clear();
        for (const auto& icon : icons_)
            descriptors_.push_back(icon.collection ? std::nullopt : host_.describe(icon.definitionId));
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(backgroundColour);
        // The level shown: the Sound file, or the collection open, Escape
        // going back up.
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::label());
        graphics.drawText(level_.has_value() ? "Sound file / " + juceString(levelName_) + "  (Escape: up)" : juce::String{"Sound file"},
            getLocalBounds().removeFromTop(header).reduced(margin, 0), juce::Justification::centredLeft);
        if (icons_.empty())
        {
            graphics.setColour(secondaryTextColour);
            graphics.setFont(WorkbenchType::body());
            graphics.drawText("No Sounds yet: drag one in from the Sound Browser or make one from the Prototypes",
                getLocalBounds().reduced(margin), juce::Justification::centred);
            return;
        }
        for (std::size_t index = 0; index < icons_.size(); ++index)
        {
            const auto cell = cellBounds(index);
            const auto selected = std::ranges::find(selection_, icons_[index].definitionId) != selection_.end();
            const auto icon = iconArea(cell);
            if (icons_[index].collection)
            {
                // A Sound collection: a folder of Sounds.
                juce::Path folder;
                folder.addRoundedRectangle(icon.withTrimmedTop(6.0F), 4.0F);
                folder.addRoundedRectangle(icon.withWidth(icon.getWidth() * 0.4F).withHeight(10.0F), 3.0F);
                graphics.setColour(panelColour);
                graphics.fillPath(folder);
                graphics.setColour(selected ? selectionColour : workbenchTheme.interactiveOutline);
                graphics.strokePath(folder, juce::PathStrokeType{selected ? 2.0F : 1.0F});
                drawName(graphics, cell, icon, icons_[index].name, selected);
                continue;
            }
            const auto& id = icons_[index].definitionId;
            const auto classIcon = host_.classIcon ? host_.classIcon(id) : std::nullopt;
            auto shape = descriptors_[index].has_value() ? talas::app::soundIconShape(*descriptors_[index]) : talas::app::SoundIconShape{};
            if (classIcon.has_value())
                shape = talas::app::soundClassIconShape(shape, *classIcon);
            const auto outline = soundIconOutline(icon, shape);
            graphics.setColour(panelColour);
            graphics.fillPath(outline);
            // The Sound being edited, patterned grey.
            if (icons_[index].beingEdited)
            {
                juce::Graphics::ScopedSaveState saved{graphics};
                graphics.reduceClipRegion(outline);
                graphics.setColour(juce::Colours::grey.withAlpha(0.55F));
                for (auto x = icon.getX() - icon.getHeight(); x < icon.getRight(); x += 6.0F)
                    graphics.drawLine(x, icon.getBottom(), x + icon.getHeight(), icon.getY(), 1.5F);
            }
            graphics.setColour(selected ? selectionColour : workbenchTheme.interactiveOutline);
            graphics.strokePath(outline, juce::PathStrokeType{selected ? 2.0F : 1.0F});
            const auto image = classIcon.has_value() && classIcon->png && host_.classImage ? host_.classImage(id) : juce::Image{};
            if (image.isValid())
                graphics.drawImage(image, icon.reduced(10.0F, 6.0F), juce::RectanglePlacement::centred);
            // Small icons draw a class's small icon.
            else if (!classIcon.has_value()
                || !drawSoundIconPicture(graphics, icon.reduced(10.0F, 6.0F), *classIcon,
                    !talas::app::soundIconDrawsSmall(*classIcon, largeIcons_), secondaryTextColour))
            {
                if (host_.glyphs)
                    drawSoundIconGlyphs(graphics, icon.reduced(8.0F, 10.0F), host_.glyphs(id));
                else if (descriptors_[index].has_value())
                    drawSoundIconGlyphs(graphics, icon.reduced(8.0F, 10.0F), talas::app::soundIconGlyphs(*descriptors_[index]));
            }
            drawName(graphics, cell, icon, icons_[index].name, selected);
        }
        if (box_.has_value())
        {
            graphics.setColour(selectionColour.withAlpha(0.15F));
            graphics.fillRect(*box_);
            graphics.setColour(selectionColour);
            graphics.drawRect(*box_, 1);
        }
    }

    static void drawName(juce::Graphics& graphics, juce::Rectangle<int> cell, juce::Rectangle<float> icon,
        const std::string& text, bool selected)
    {
        const auto name = cell.withTrimmedTop(static_cast<int>(icon.getBottom()) - cell.getY() + 4).withHeight(18);
        if (selected)
        {
            graphics.setColour(selectionColour.withAlpha(0.3F));
            graphics.fillRoundedRectangle(name.toFloat(), 3.0F);
        }
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::label());
        graphics.drawFittedText(juceString(text), name, juce::Justification::centred, 1);
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        grabKeyboardFocus();
        if (event.mods.isPopupMenu())
        {
            if (const auto index = iconAt(event.getPosition());
                index.has_value() && std::ranges::find(selection_, icons_[*index].definitionId) == selection_.end())
                selection_ = {icons_[*index].definitionId};
            repaint();
            showActions();
            return;
        }
        press(event.getPosition(), event.mods.isShiftDown());
    }

    // The Action menu's operations on the selection.
    void showActions()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Collect...", !selection_.empty());
        menu.addItem(2, "Open", selection_.size() == 1);
        menu.addItem(3, "Rename...", selection_.size() == 1);
        menu.addItem(4, "Duplicate", !selection_.empty());
        menu.addItem(5, "Delete", !selection_.empty());
        menu.addItem(6, "Expand", !selected(false).empty());
        juce::Component::SafePointer<SoundFilePanelImpl> safeThis(this);
        menu.showMenuAsync(juce::PopupMenu::Options{}, [safeThis](int chosen) {
            if (safeThis == nullptr)
                return;
            if (chosen == 1)
                safeThis->collect(std::nullopt);
            else if (chosen == 2)
                safeThis->open(safeThis->selection_.front());
            else if (chosen == 3)
                (void)safeThis->keyPressed(juce::KeyPress{juce::KeyPress::returnKey});
            else if (chosen == 4)
                (void)safeThis->keyPressed(juce::KeyPress{'d', juce::ModifierKeys::commandModifier, 0});
            else if (chosen == 5)
                (void)safeThis->keyPressed(juce::KeyPress{juce::KeyPress::deleteKey});
            else if (chosen == 6)
                (void)safeThis->keyPressed(juce::KeyPress{'e', juce::ModifierKeys::commandModifier, 0});
        });
    }

    [[nodiscard]] bool isCollection(const std::string& id) const
    {
        return std::ranges::any_of(icons_, [&](const talas::app::SoundFileIcon& icon) { return icon.collection && icon.definitionId == id; });
    }

    // The selection's Sounds or its collections.
    [[nodiscard]] std::vector<std::string> selected(bool collections) const
    {
        std::vector<std::string> ids;
        for (const auto& id : selection_)
        {
            if (isCollection(id) == collections)
                ids.push_back(id);
        }
        return ids;
    }

    // A double-click opens a Sound's editor, or a collection's Sounds; on
    // the Sound being edited it brings its editor to the front.
    void open(const std::string& id)
    {
        const auto icon = std::ranges::find(icons_, id, &talas::app::SoundFileIcon::definitionId);
        const auto click = icon == icons_.end() ? talas::app::SoundFileDoubleClick::EditSound : talas::app::soundFileDoubleClick(*icon);
        if (click == talas::app::SoundFileDoubleClick::BringEditorToFront && host_.bringEditorToFront)
        {
            host_.bringEditorToFront(id);
            return;
        }
        if (click != talas::app::SoundFileDoubleClick::OpenCollection)
        {
            host_.open(id);
            return;
        }
        level_ = id;
        selection_.clear();
        refresh();
    }

    // Collect...: the selection into a new collection here.
    void collect(const std::optional<std::string>& name)
    {
        if (selection_.empty())
            return;
        host_.collect(level_, selected(false), selected(true), name);
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (boxStart_.has_value())
        {
            box_ = juce::Rectangle<int>{*boxStart_, event.getPosition()};
            repaint();
            return;
        }
        // With Control or Option held, the Sounds themselves move.
        if (event.mods.isAltDown() || event.mods.isCtrlDown())
        {
            if (event.getDistanceFromDragStart() > 6 && iconAt(event.getMouseDownPosition()).has_value())
            {
                moving_ = true;
                pendingSingle_.reset();
            }
            return;
        }
        // A Sound's icon dragged out is a copy of it, into the
        // Workbench's signal flow or Timeline.
        if (event.getDistanceFromDragStart() > 6 && !isDragAndDropActive())
        {
            const auto description = dragDescriptionForSmoke(event.getMouseDownPosition());
            pendingSingle_.reset();
            if (!description.isVoid())
                startDragging(description, this, juce::ScaledImage{}, true);
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (std::exchange(moving_, false))
        {
            moveTo(event.getPosition());
            return;
        }
        release(event.getPosition(), event.mods.isShiftDown());
    }

    // The selected Sounds before the Sound at a point; past a collection's
    // icon, before this level's first Sound; on the background, last.
    void moveTo(juce::Point<int> point)
    {
        const auto sounds = selected(false);
        if (sounds.empty() || !host_.move)
            return;
        std::string before;
        if (const auto index = iconAt(point))
        {
            before = icons_[*index].definitionId;
            if (isCollection(before))
            {
                const auto first = std::ranges::find(icons_, false, &talas::app::SoundFileIcon::collection);
                before = first == icons_.end() ? std::string{} : first->definitionId;
            }
        }
        if (std::ranges::find(sounds, before) != sounds.end())
            return;
        host_.move(sounds, before);
    }

    void moveForSmoke(juce::Point<int> to) override { moveTo(to); }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        if (const auto index = iconAt(event.getPosition()))
            open(icons_[*index].definitionId);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const auto first = selection_.empty() ? std::string{} : selection_.front();
        const auto mods = key.getModifiers();
        const auto letter = juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(key.getKeyCode()));
        if (key.getKeyCode() == juce::KeyPress::escapeKey && level_.has_value())
        {
            // Back up to the level holding the collection open.
            const auto shown = *level_;
            level_ = host_.parentOf(shown);
            selection_ = {shown};
            refresh();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::spaceKey && !mods.isCommandDown())
        {
            // Ctrl+Space plays the selected Sound, several (or a collection)
            // one after another; Space pauses and resumes.
            if (first.empty())
                return true;
            if (mods.isCtrlDown() && (selection_.size() > 1 || isCollection(first)) && host_.playInTurn)
                host_.playInTurn(talas::app::soundFileSelectionInOrder(icons_, selection_));
            else if (!isCollection(first))
                mods.isCtrlDown() ? host_.play(first) : host_.pauseOrResume(first);
            return true;
        }
        if (mods.isCommandDown() && letter == 'K')
        {
            host_.stop();
            return true;
        }
        if (key.getKeyCode() == juce::KeyPress::returnKey && !first.empty())
        {
            // Enter renames the selected Sound, or collection.
            isCollection(first) ? host_.renameCollection(first) : host_.rename(first);
            return true;
        }
        if ((key.getKeyCode() == juce::KeyPress::deleteKey || key.getKeyCode() == juce::KeyPress::backspaceKey)
            && !selection_.empty())
        {
            const auto sounds = selected(false);
            const auto collections = selected(true);
            if (!collections.empty())
                host_.removeCollections(collections);
            if (!sounds.empty())
                host_.remove(sounds);
            return true;
        }
        if (mods.isCommandDown() && letter == 'D' && !selection_.empty())
        {
            const auto sounds = selected(false);
            const auto collections = selected(true);
            if (!sounds.empty())
                host_.duplicate(sounds);
            if (!collections.empty())
                host_.duplicateCollections(collections);
            return true;
        }
        if (mods.isCommandDown() && letter == 'C' && !selected(false).empty() && host_.copy)
        {
            host_.copy(selected(false));
            return true;
        }
        if (mods.isCommandDown() && letter == 'V' && host_.paste)
        {
            host_.paste();
            return true;
        }
        if (mods.isCommandDown() && letter == 'E' && !selected(false).empty())
        {
            // Expand, a copy of each Sound expanded, beside it.
            host_.expand(selected(false));
            return true;
        }
        if (mods.isCommandDown() && letter == 'A')
        {
            selection_.clear();
            for (const auto& icon : icons_)
                selection_.push_back(icon.definitionId);
            repaint();
            return true;
        }
        return false;
    }

    [[nodiscard]] std::vector<talas::app::SoundFileIcon> iconsForSmoke() const override { return icons_; }
    [[nodiscard]] std::vector<std::string> selectionForSmoke() const override { return selection_; }
    [[nodiscard]] std::vector<std::string> selectedSounds() const override { return selected(false); }

    void selectSounds(const std::vector<std::string>& definitionIds) override
    {
        level_.reset();
        selection_ = definitionIds;
        refresh();
    }

    [[nodiscard]] std::optional<juce::Rectangle<int>> iconBoundsForSmoke(const std::string& definitionId) const override
    {
        const auto found = std::ranges::find(icons_, definitionId, &talas::app::SoundFileIcon::definitionId);
        if (found == icons_.end())
            return std::nullopt;
        return iconArea(cellBounds(static_cast<std::size_t>(found - icons_.begin()))).toNearestInt();
    }

    void clickForSmoke(juce::Point<int> point, bool shift, bool twice) override
    {
        press(point, shift);
        release(point, shift);
        if (twice)
        {
            if (const auto index = iconAt(point))
                open(icons_[*index].definitionId);
        }
    }

    [[nodiscard]] std::optional<std::string> levelForSmoke() const override
    {
        return level_;
    }

    void collectForSmoke(const std::string& name) override
    {
        collect(name);
    }

    void boxForSmoke(juce::Point<int> from, juce::Point<int> to, bool shift) override
    {
        press(from, shift);
        if (boxStart_.has_value())
            box_ = juce::Rectangle<int>{from, to};
        release(to, shift);
    }

    [[nodiscard]] juce::var dragDescriptionForSmoke(juce::Point<int> point) const override
    {
        const auto index = iconAt(point);
        if (!index.has_value())
            return {};
        return juceString(soundDragPrefix) + juceString(icons_[*index].definitionId);
    }

    bool keyForSmoke(const juce::KeyPress& key) override { return keyPressed(key); }

    void setLargeIcons(bool large) override
    {
        largeIcons_ = large;
        repaint();
    }

    [[nodiscard]] bool largeIcons() const override { return largeIcons_; }

private:
    [[nodiscard]] int cellWidth() const noexcept { return largeIcons_ ? 124 : 84; }
    [[nodiscard]] int cellHeight() const noexcept { return largeIcons_ ? 84 : 58; }

    [[nodiscard]] juce::Rectangle<int> cellBounds(std::size_t index) const
    {
        const auto columns = talas::app::soundFileColumns(getWidth() - 2 * margin, cellWidth());
        const auto cell = talas::app::soundFileCell(index, columns);
        return {margin + static_cast<int>(cell.column) * cellWidth(), header + margin + static_cast<int>(cell.row) * cellHeight(),
            cellWidth(), cellHeight()};
    }

    [[nodiscard]] juce::Rectangle<float> iconArea(juce::Rectangle<int> cell) const
    {
        return cell.toFloat()
            .withSizeKeepingCentre(largeIcons_ ? 80.0F : 48.0F, largeIcons_ ? 44.0F : 26.0F)
            .withY(static_cast<float>(cell.getY()) + 6.0F);
    }

    [[nodiscard]] std::optional<std::size_t> iconAt(juce::Point<int> point) const
    {
        for (std::size_t index = 0; index < icons_.size(); ++index)
        {
            // The icon and its name below.
            const auto icon = iconArea(cellBounds(index));
            if (icon.withHeight(icon.getHeight() + 24.0F).contains(point.toFloat()))
                return index;
        }
        return std::nullopt;
    }

    void press(juce::Point<int> point, bool shift)
    {
        boxStart_.reset();
        box_.reset();
        const auto index = iconAt(point);
        if (!index.has_value())
        {
            // A box drawn in the background selects the Sounds
            // within it.
            boxStart_ = point;
            boxSelection_ = selection_;
            selection_ = talas::app::soundFileClick(std::move(selection_), {}, shift);
            repaint();
            return;
        }
        const auto& id = icons_[*index].definitionId;
        // A click on a selected Sound keeps the selection to drag it, and
        // selects that Sound alone when released without a drag.
        pendingSingle_.reset();
        if (shift || std::ranges::find(selection_, id) == selection_.end())
            selection_ = talas::app::soundFileClick(std::move(selection_), id, shift);
        else
            pendingSingle_ = id;
        repaint();
    }

    void release(juce::Point<int>, bool shift)
    {
        if (pendingSingle_.has_value())
        {
            selection_ = talas::app::soundFileClick(std::move(selection_), *pendingSingle_, false);
            pendingSingle_.reset();
            repaint();
        }
        if (!boxStart_.has_value())
            return;
        if (box_.has_value())
        {
            std::vector<std::string> within;
            for (std::size_t index = 0; index < icons_.size(); ++index)
            {
                if (box_->intersects(iconArea(cellBounds(index)).toNearestInt()))
                    within.push_back(icons_[index].definitionId);
            }
            selection_ = talas::app::soundFileBoxSelection(boxSelection_, within, shift);
        }
        boxStart_.reset();
        box_.reset();
        repaint();
    }

    // A prototype dragged in from the Prototypes is a new Sound.
    [[nodiscard]] static std::optional<std::string> prototypeOf(const juce::var& description)
    {
        const auto text = description.toString().toStdString();
        if (!text.starts_with(prototypeDragPrefix))
            return std::nullopt;
        return text.substr(prototypeDragPrefix.size());
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        const auto text = details.description.toString().toStdString();
        return prototypeOf(details.description).has_value() || talas::app::soundBrowserDrag(text).has_value()
            || talas::app::signalFlowSoundDrag(text).has_value();
    }

    void itemDropped(const SourceDetails& details) override { (void) dropForSmoke(details.description); }

    bool dropForSmoke(const juce::var& description) override
    {
        // A Sound or a file from the Sound
        // Browser, or a Sound out of a signal flow.
        const auto text = description.toString().toStdString();
        if ((talas::app::soundBrowserDrag(text).has_value() || talas::app::signalFlowSoundDrag(text).has_value()) && host_.addDragged)
        {
            host_.addDragged(text);
            return true;
        }
        const auto typeId = prototypeOf(description);
        if (!typeId || !host_.addPrototype)
            return false;
        host_.addPrototype(*typeId);
        return true;
    }

    SoundFileHost host_;
    std::vector<talas::app::SoundFileIcon> icons_;
    bool largeIcons_ = true;
    std::vector<std::optional<talas::core::NodeTypeDescriptor>> descriptors_;
    std::vector<std::string> selection_;
    std::vector<std::string> boxSelection_;
    std::optional<std::string> pendingSingle_;
    bool moving_ = false;
    std::optional<std::string> level_;
    std::string levelName_;
    std::optional<juce::Point<int>> boxStart_;
    std::optional<juce::Rectangle<int>> box_;
};
}

std::unique_ptr<SoundFilePanel> makeSoundFilePanel(SoundFileHost host)
{
    return std::make_unique<SoundFilePanelImpl>(std::move(host));
}
}
