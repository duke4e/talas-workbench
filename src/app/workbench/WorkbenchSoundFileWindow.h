#pragma once

#include "talas/app/SoundBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/SoundIcons.h"
#include "talas/core/NodeRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// What the Sound file window asks of the Workbench: the project's Sounds and
// their kinds, a Sound's editor (a double-click), playing (Ctrl+Space),
// pausing and resuming (Space), stopping (Ctrl+K), renaming (Enter),
// deleting, duplicating (Ctrl+D) and expanding (Ctrl+E) the selected Sounds.
struct SoundFileHost
{
    // The icons of a level: the top level, or a Sound collection's.
    std::function<std::vector<talas::app::SoundFileIcon>(const std::optional<std::string>&)> icons;
    std::function<std::optional<std::string>(const std::string&)> parentOf;
    // A collection's name, none when there is no such collection.
    std::function<std::optional<std::string>(const std::string&)> collectionName;
    std::function<std::optional<talas::core::NodeTypeDescriptor>(const std::string&)> describe;
    std::function<void(const std::string&)> open;
    // A double-click on the Sound being edited:
    // its Sound editor to the front, as it is.
    std::function<void(const std::string&)> bringEditorToFront;
    std::function<void(const std::string&)> play;
    // Several Sounds or a collection played: one
    // after another, in the order their icons lie.
    std::function<void(const std::vector<std::string>&)> playInTurn;
    std::function<void(const std::string&)> pauseOrResume;
    std::function<void()> stop;
    std::function<void(const std::string&)> rename;
    std::function<void(const std::vector<std::string>&)> remove;
    std::function<void(const std::vector<std::string>&)> duplicate;
    // Copy and Paste (Ctrl+C, Ctrl+V): the Sounds selected onto the
    // clipboard, and what it
    // holds pasted as Sounds of the project.
    std::function<void(const std::vector<std::string>&)> copy;
    // The Sounds dragged with Control or Option held
    // move, just before the Sound dropped on, or last.
    std::function<void(const std::vector<std::string>&, const std::string&)> move;
    std::function<void()> paste;
    // Expand (Ctrl+E): a copy of each Sound with
    // its Sounds expanded one level, the Sound itself unchanged.
    std::function<void(const std::vector<std::string>&)> expand;
    // A prototype dragged in from the Prototypes: a
    // new Sound of it.
    std::function<void(const std::string&)> addPrototype;
    // A Sound or a file dragged in from a Sound Browser: a new Sound of it,
    // as the drag describes it.
    std::function<void(const std::string&)> addDragged;
    // A class's own icon: its drawn dots and PNG
    // picture, and the inner icon it shows, its own kind's or another's.
    std::function<std::optional<talas::project::ProjectSoundIcon>(const std::string&)> classIcon;
    std::function<juce::Image(const std::string&)> classImage;
    std::function<std::vector<talas::app::IconGlyph>(const std::string&)> glyphs;
    // Sound collections: Collect
    // the selection at a level (asking the name when none is given),
    // rename, delete and duplicate collections.
    std::function<void(const std::optional<std::string>&, const std::vector<std::string>&,
        const std::vector<std::string>&, const std::optional<std::string>&)>
        collect;
    std::function<void(const std::string&)> renameCollection;
    std::function<void(const std::vector<std::string>&)> removeCollections;
    std::function<void(const std::vector<std::string>&)> duplicateCollections;
};

class SoundFilePanel : public juce::Component
{
public:
    ~SoundFilePanel() override = default;

    // The project's Sounds changed.
    virtual void refresh() = 0;
    // Edit > View...: large or small icons.
    virtual void setLargeIcons(bool large) = 0;
    [[nodiscard]] virtual bool largeIcons() const = 0;
    // The selected Sounds, not collections, that the Action and Info menus
    // take while the window is in front.
    [[nodiscard]] virtual std::vector<std::string> selectedSounds() const = 0;
    // The Sounds of the top level given as the selection, highlighted, as a
    // class's new instance is.
    virtual void selectSounds(const std::vector<std::string>& definitionIds) = 0;

    [[nodiscard]] virtual std::vector<talas::app::SoundFileIcon> iconsForSmoke() const = 0;
    [[nodiscard]] virtual std::vector<std::string> selectionForSmoke() const = 0;
    [[nodiscard]] virtual std::optional<juce::Rectangle<int>> iconBoundsForSmoke(const std::string& definitionId) const = 0;
    // A click at a point, with Shift or not; a double-click opens.
    virtual void clickForSmoke(juce::Point<int> point, bool shift, bool twice = false) = 0;
    // A box drawn in the background from one point to another.
    virtual void boxForSmoke(juce::Point<int> from, juce::Point<int> to, bool shift) = 0;
    [[nodiscard]] virtual juce::var dragDescriptionForSmoke(juce::Point<int> point) const = 0;
    virtual bool keyForSmoke(const juce::KeyPress& key) = 0;
    // A drag's description dropped on the window; whether it took it.
    virtual bool dropForSmoke(const juce::var& description) = 0;
    // The Sounds selected moved, as by a Control or Option drag, to a point.
    virtual void moveForSmoke(juce::Point<int> to) = 0;
    // The collection shown, none for the top level; Collect with a name.
    [[nodiscard]] virtual std::optional<std::string> levelForSmoke() const = 0;
    virtual void collectForSmoke(const std::string& name) = 0;
};

[[nodiscard]] std::unique_ptr<SoundFilePanel> makeSoundFilePanel(SoundFileHost host);
}
