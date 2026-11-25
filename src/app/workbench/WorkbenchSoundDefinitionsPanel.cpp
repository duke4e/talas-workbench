#include "app/workbench/WorkbenchSoundDefinitionsPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/core/FromChars.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] std::string_view sourceLabel(
    talas::app::SoundLibrarySourceKind source) noexcept
{
    switch (source)
    {
    case talas::app::SoundLibrarySourceKind::ProjectDefinition:
        return "PROJECT";
    case talas::app::SoundLibrarySourceKind::Component:
        return "COMPONENT";
    case talas::app::SoundLibrarySourceKind::OriginalExample:
        return "EXAMPLE";
    }
    return "UNKNOWN";
}

[[nodiscard]] std::string_view compatibilityLabel(
    talas::app::SoundLibraryCompatibility compatibility) noexcept
{
    switch (compatibility)
    {
    case talas::app::SoundLibraryCompatibility::Ready:
        return "Ready";
    case talas::app::SoundLibraryCompatibility::ValidationOnly:
        return "Validation only";
    case talas::app::SoundLibraryCompatibility::MissingDependency:
        return "Missing dependency";
    case talas::app::SoundLibraryCompatibility::MissingAsset:
        return "Missing asset";
    case talas::app::SoundLibraryCompatibility::UnsupportedRuntime:
        return "Unsupported runtime";
    case talas::app::SoundLibraryCompatibility::PreservedUnavailable:
        return "Preserved unavailable";
    case talas::app::SoundLibraryCompatibility::Malformed:
        return "Malformed";
    }
    return "Unavailable";
}

// The row's second line: its source, and for a demo its kind and what it
// needs, then compatibility and category.
[[nodiscard]] juce::String entryDetailLine(
    const talas::app::SoundLibraryEntry& entry)
{
    const auto separator = juceString("  ·  ");
    if (entry.demoKind == talas::app::SoundLibraryDemoKind::None)
    {
        return juceString(sourceLabel(entry.sourceKind)) + separator
            + juceString(compatibilityLabel(entry.compatibility)) + separator
            + juceString(entry.category);
    }
    return juce::String{"DEMO"} + separator
        + juceString(talas::app::soundLibraryDemoKindLabel(entry.demoKind))
        + separator + juceString(compatibilityLabel(entry.compatibility))
        + (entry.demoKind == talas::app::SoundLibraryDemoKind::Effect
                ? separator + "needs live input"
                : separator + "plays a demo phrase");
}

// The detail pane's second line for a demo: what it is and how to hear it.
[[nodiscard]] juce::String demoGuidance(
    const talas::app::SoundLibraryEntry& entry)
{
    switch (entry.demoKind)
    {
    case talas::app::SoundLibraryDemoKind::Effect:
        return "Demo effect";
    case talas::app::SoundLibraryDemoKind::Instrument:
        return juceString(
            "Demo instrument · Audition plays its demo phrase; open it to "
            "play it from a MIDI keyboard");
    case talas::app::SoundLibraryDemoKind::None:
        break;
    }
    return {};
}

[[nodiscard]] std::string_view auditionPortTypeLabel(
    talas::core::PortType type) noexcept
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return "Audio";
    case talas::core::PortType::Control:
        return "Control";
    case talas::core::PortType::Event:
        return "Event — unsupported";
    case talas::core::PortType::SampleBuffer:
        return "Sample buffer — unsupported";
    case talas::core::PortType::Wavetable:
        return "Wavetable — unsupported";
    case talas::core::PortType::Spectrum:
        return "Spectrum — unsupported";
    case talas::core::PortType::ComplexSpectrum:
        return "Complex spectrum — unsupported";
    case talas::core::PortType::NoteStream:
        return "Note stream — unsupported";
    }
    return "Unsupported";
}

[[nodiscard]] std::optional<double> parseFiniteNumber(
    std::string_view text) noexcept
{
    double value = 0.0;
    const auto parsed = talas::fromChars(
        text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{}
        || parsed.ptr != text.data() + text.size()
        || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

class SoundDefinitionsPanelImpl final : public SoundDefinitionsPanel,
                                        private juce::ListBoxModel
{
  public:
    using ActionCallback = std::function<void(Action, std::string)>;
    explicit SoundDefinitionsPanelImpl(ActionCallback onAction)
        : onAction_(std::move(onAction)), createButton_("New"),
          renameButton_("Rename"), deleteButton_("Delete Sound"),
          addButton_("Use"), extractButton_("Extract"), inlineButton_("Inline"),
          parentButton_("Parent"), auditionButton_("Audition"),
          routeButton_("Control"),
          bookmarkButton_("Bookmark View"), restoreButton_("Restore View"),
          showButton_("Show in Structure"), favoriteButton_("Favorite"),
          openExampleButton_("Open Example"), helpButton_("Open Help")
    {
        searchInput_.setComponentID("library.sound.search");
        scopeBox_.setComponentID("library.sound.collection");
        sourceBox_.setComponentID("library.sound.source");
        compatibilityBox_.setComponentID("library.sound.compatibility");
        healthBox_.setComponentID("library.sound.health");
        categoryBox_.setComponentID("library.sound.category");
        originBox_.setComponentID("library.sound.origin");
        list_.setComponentID("library.sound.results");
        detailLabel_.setComponentID("library.sound.details");
        auditionInputBox_.setComponentID("library.sound.audition.input");
        auditionSourceBox_.setComponentID("library.sound.audition.source");
        auditionValueBox_.setComponentID("library.sound.audition.value");
        createButton_.setComponentID("library.sound.action.create");
        renameButton_.setComponentID("library.sound.action.rename");
        deleteButton_.setComponentID("library.sound.action.delete");
        addButton_.setComponentID("library.sound.action.use");
        extractButton_.setComponentID("library.sound.action.extract");
        inlineButton_.setComponentID("library.sound.action.inline");
        parentButton_.setComponentID("library.sound.action.parent");
        auditionButton_.setComponentID("library.sound.action.audition");
        routeButton_.setComponentID("library.sound.action.control-project");
        bookmarkButton_.setComponentID("library.sound.action.bookmark");
        restoreButton_.setComponentID("library.sound.action.restore");
        showButton_.setComponentID("library.sound.action.show");
        favoriteButton_.setComponentID("library.sound.action.favorite");
        openExampleButton_.setComponentID("library.sound.action.open-example");
        helpButton_.setComponentID("library.sound.action.help");
        searchInput_.setTextToShowWhenEmpty(
            "Search Sounds",
            secondaryTextColour);
        searchInput_.setFont(WorkbenchType::body());
        searchInput_.onTextChange = [this] {
            rebuildVisibleDefinitions();
        };
        scopeBox_.addItem("All", 1);
        scopeBox_.addItem("Favorites", 2);
        scopeBox_.addItem("Recent", 3);
        scopeBox_.setSelectedId(1, juce::dontSendNotification);
        scopeBox_.onChange = [this] {
            libraryState_.collection = static_cast<
                talas::project::WorkspaceSoundLibraryCollection>(
                    std::max(1, scopeBox_.getSelectedId()) - 1);
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        for (const auto& label : {"All sources", "Project Sounds", "Components", "Original examples"})
            sourceBox_.addItem(label, sourceBox_.getNumItems() + 1);
        for (const auto& label : {"All compatibility", "Ready", "Needs attention", "Unavailable"})
            compatibilityBox_.addItem(label, compatibilityBox_.getNumItems() + 1);
        for (const auto& label : {"All health", "Healthy", "Warning", "Unavailable"})
            healthBox_.addItem(label, healthBox_.getNumItems() + 1);
        categoryBox_.addItem("All categories", 1);
        originBox_.addItem("All origins", 1);
        sourceBox_.setSelectedId(1, juce::dontSendNotification);
        compatibilityBox_.setSelectedId(1, juce::dontSendNotification);
        healthBox_.setSelectedId(1, juce::dontSendNotification);
        categoryBox_.setSelectedId(1, juce::dontSendNotification);
        originBox_.setSelectedId(1, juce::dontSendNotification);
        sourceBox_.onChange = [this] {
            libraryState_.sourceFilter = static_cast<
                talas::project::WorkspaceSoundLibrarySourceFilter>(
                    std::max(1, sourceBox_.getSelectedId()) - 1);
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        compatibilityBox_.onChange = [this] {
            libraryState_.compatibilityFilter = static_cast<
                talas::project::WorkspaceSoundLibraryCompatibilityFilter>(
                    std::max(1, compatibilityBox_.getSelectedId()) - 1);
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        healthBox_.onChange = [this] {
            libraryState_.healthFilter = static_cast<
                talas::project::WorkspaceSoundLibraryHealthFilter>(
                    std::max(1, healthBox_.getSelectedId()) - 1);
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        categoryBox_.onChange = [this] {
            libraryState_.category = categoryBox_.getSelectedId() <= 1
                ? std::optional<std::string>{}
                : std::optional<std::string>{
                    categoryBox_.getText().toStdString()};
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        originBox_.onChange = [this] {
            libraryState_.origin = originBox_.getSelectedId() <= 1
                ? std::optional<std::string>{}
                : std::optional<std::string>{
                    originBox_.getText().toStdString()};
            rebuildVisibleDefinitions();
            publishLibraryState();
        };
        list_.setModel(this);
        list_.setRowHeight(34);
        list_.setColour(
            juce::ListBox::backgroundColourId,
            panelColour);
        breadcrumbViewport_.setViewedComponent(&breadcrumbContainer_, false);
        breadcrumbViewport_.setScrollBarsShown(false, true);
        breadcrumbViewport_.setScrollBarThickness(8);
        describeControl(
            list_,
            "Reusable Sounds",
            "Choose a reusable Sound to open in the opposite Structure "
            "pane, edit, instantiate, keep, or audition.");
        describeControl(searchInput_,
            "Search reusable Sounds",
            "Filters reusable Sounds by display name or stable definition id.");
        describeControl(
            scopeBox_,
            "Sound Library collection",
            "Shows all, favorite, or recently selected Sound Library entries.");
        describeControl(sourceBox_,
            "Sound Library source filter",
            "Filters exact project Sounds, components, or repository-original examples.");
        describeControl(compatibilityBox_,
            "Sound Library compatibility filter",
            "Filters Ready, attention-required, or unavailable entries from model authority.");
        describeControl(healthBox_,
            "Sound Library health filter",
            "Filters dependency and asset health without guessing from labels.");
        describeControl(categoryBox_,
            "Sound Library category filter",
            "Filters the exact model-derived category.");
        describeControl(originBox_,
            "Sound Library origin filter",
            "Filters the exact manifest or descriptor origin.");
        describeControl(favoriteButton_,
            "Toggle Sound Library favorite",
            "Changes only the local WorkspaceSession favorite collection.");
        describeControl(openExampleButton_,
            "Open original example",
            "Opens only the exact repository-original project named by the manifest.");
        describeControl(helpButton_,
            "Open exact Sound Library help",
            "Opens the manifest or descriptor help anchor for the selected entry.");
        describeControl(bookmarkButton_,
            "Bookmark hierarchy view",
            "Adds or removes the selected reusable Sound and current "
            "split configuration from local workspace bookmarks.");
        describeControl(restoreButton_,
            "Restore hierarchy bookmark",
            "Restores the selected reusable Sound hierarchy path, split, pane "
            "order, and graph view from the local workspace session.");
        describeControl(showButton_,
            "Show reusable Sound in Structure",
            "Opens the selected reusable Sound in the opposite "
            "Structure pane without changing project topology.");
        describeControl(auditionButton_,
            "Audition reusable Sound",
            "Stages an isolated prepared preview with bounded local "
            "input substitutes without changing project topology.");
        describeControl(routeButton_,
            "Control current project",
            "Runs a selected controller example and routes its generated "
            "Event Value to a matching prepared VCS control in the current project.");
        describeControl(auditionInputBox_,
            "Private audition input",
            "Selects one exact reusable Sound input without changing project topology.");
        describeControl(auditionSourceBox_,
            "Private audition source",
            "Chooses only source kinds compatible with the selected exact input type.");
        describeControl(auditionValueBox_,
            "Private audition source value",
            "Chooses a bounded sine frequency, control constant, or verified frozen Audio asset.");
        describeControl(parentButton_,
            "Open parent Sound",
            "Moves one level toward Project Root while retaining per-definition selection.");
        addAndMakeVisible(searchInput_);
        addAndMakeVisible(scopeBox_);
        addAndMakeVisible(sourceBox_);
        addAndMakeVisible(compatibilityBox_);
        addAndMakeVisible(healthBox_);
        addAndMakeVisible(categoryBox_);
        addAndMakeVisible(originBox_);
        addAndMakeVisible(list_);
        addAndMakeVisible(breadcrumbViewport_);
        addAndMakeVisible(detailLabel_);
        addAndMakeVisible(auditionInputBox_);
        addAndMakeVisible(auditionSourceBox_);
        addAndMakeVisible(auditionValueBox_);
        for (auto* button : {
                 &createButton_,
                 &renameButton_,
                 &deleteButton_,
                 &addButton_,
                 &extractButton_,
                 &inlineButton_,
                 &parentButton_,
                 &auditionButton_,
                 &routeButton_,
                 &bookmarkButton_,
                 &restoreButton_,
                 &showButton_,
                 &favoriteButton_,
                 &openExampleButton_,
                 &helpButton_,
             })
        {
            addAndMakeVisible(*button);
        }
        createButton_.onClick = [this] { onAction_(Action::Create, {}); };
        renameButton_.onClick = [this]
        { onAction_(Action::Rename, selectedDefinitionId()); };
        deleteButton_.onClick = [this]
        { onAction_(Action::Delete, selectedDefinitionId()); };
        addButton_.onClick = [this]
        { onAction_(Action::UseInProject, selectedEntryId()); };
        extractButton_.onClick = [this]
        { onAction_(Action::ExtractSelection, {}); };
        inlineButton_.onClick = [this]
        { onAction_(Action::InlineSelection, {}); };
        auditionButton_.onClick = [this] {
            const auto* entry = selectedEntry();
            if (entry != nullptr
                && actionAvailable(
                    *entry, talas::app::SoundLibraryAction::Audition))
            {
                onAction_(Action::Audition, entry->stableId);
                return;
            }
            if (auditionDefinitionId_.has_value())
                onAction_(Action::Audition, *auditionDefinitionId_);
        };
        routeButton_.onClick = [this] {
            const auto* entry = selectedEntry();
            if (entry != nullptr && actionAvailable(
                    *entry, talas::app::SoundLibraryAction::Audition)
                && std::ranges::find(entry->tags, "controller-route")
                    != entry->tags.end())
                onAction_(Action::ControlCurrentProject, entry->stableId);
            else if (controlsCurrentProject_
                && auditionDefinitionId_.has_value())
                onAction_(Action::ControlCurrentProject,
                    *auditionDefinitionId_);
        };
        parentButton_.onClick = [this] { onAction_(Action::OpenParent, {}); };
        bookmarkButton_.onClick = [this]
        {
            const auto selected = selectedDefinitionId();
            onAction_(Action::ToggleBookmark, selected);
        };
        restoreButton_.onClick = [this]
        { onAction_(Action::RestoreBookmark, selectedDefinitionId()); };
        showButton_.onClick = [this]
        { onAction_(Action::OpenDefinition, selectedDefinitionId()); };
        favoriteButton_.onClick = [this] {
            const auto selected = selectedEntryId();
            if (selected.empty())
                return;
            const auto found = std::ranges::find(
                libraryState_.favoriteEntryIds, selected);
            if (found == libraryState_.favoriteEntryIds.end())
            {
                libraryState_.favoriteEntryIds.insert(
                    libraryState_.favoriteEntryIds.begin(), selected);
                if (libraryState_.favoriteEntryIds.size()
                    > talas::project::maximumWorkspaceSoundLibraryFavorites)
                {
                    libraryState_.favoriteEntryIds.pop_back();
                }
            }
            else
            {
                libraryState_.favoriteEntryIds.erase(found);
            }
            rebuildVisibleDefinitions(selected);
            publishLibraryState(Action::ToggleFavorite);
        };
        openExampleButton_.onClick = [this]
        { onAction_(Action::OpenExample, selectedEntryId()); };
        helpButton_.onClick = [this]
        { onAction_(Action::OpenHelp, selectedEntryId()); };
        auditionInputBox_.onChange = [this] {
            if (!refreshing_)
                rebuildAuditionSourceControls();
        };
        auditionSourceBox_.onChange = [this] {
            if (refreshing_)
                return;
            rebuildAuditionValueControls();
            commitAuditionInputSubstitution();
        };
        auditionValueBox_.onChange = [this] {
            if (!refreshing_)
                commitAuditionInputSubstitution();
        };
        detailLabel_.setFont(WorkbenchType::micro());
        detailLabel_.setColour(juce::Label::textColourId, secondaryTextColour);
        detailLabel_.setJustificationType(juce::Justification::topLeft);
        updateSelectionState();
        rebuildAuditionInputControls();
    }

    void showRootContext() override
    {
        refreshing_ = true;
        list_.deselectAllRows();
        refreshing_ = false;
        updateSelectionState();
        rebuildAuditionInputControls();
        navigationPath_.clear();
        rebuildBreadcrumbButtons();
        repaint();
    }

    void setNavigationPath(std::vector<std::string> path) override
    {
        navigationPath_ = std::move(path);
        rebuildBreadcrumbButtons();
        updateSelectionState();
    }

    void setBookmarks(const std::vector<std::string>& definitionIds) override
    {
        bookmarks_.clear();
        bookmarks_.insert(
            definitionIds.begin(),
            definitionIds.end());
        rebuildVisibleDefinitions();
    }

    void selectDefinition(std::string_view definitionId) override
    {
        const auto found = std::ranges::find_if(
            visibleEntries_,
            [&](const auto& entry) {
                return entry.definitionId
                    == std::optional<std::string>{definitionId};
            });
        if (found == visibleEntries_.end())
            return;
        refreshing_ = true;
        list_.selectRow(static_cast<int>(
            std::distance(visibleEntries_.begin(), found)));
        refreshing_ = false;
        updateSelectionState();
        rebuildAuditionInputControls();
    }

    void setAuditionState(
        std::optional<std::string> definitionId,
        bool controlsCurrentProject) override
    {
        auditionDefinitionId_ = std::move(definitionId);
        controlsCurrentProject_ = auditionDefinitionId_.has_value()
            && controlsCurrentProject;
        updateSelectionState();
        repaint();
    }

    [[nodiscard]] bool controllerRouteAvailableForSmoke() const override
    {
        return routeButton_.isEnabled();
    }

    void triggerControllerRouteActionForSmoke() override
    {
        if (routeButton_.isEnabled() && routeButton_.onClick)
            routeButton_.onClick();
    }

    [[nodiscard]] bool hasActiveAuditionState(
        std::string_view definitionId) const override
    {
        return (auditionDefinitionId_
                    == std::optional<std::string>{definitionId}
                || auditionDefinitionId_
                    == std::optional<std::string>{
                        "project:" + std::string{definitionId}})
            && auditionButton_.getToggleState()
            && auditionButton_.getButtonText() == "Stop Preview"
            && auditionButton_.getTitle()
                == "Stop isolated Sound audition";
    }

    [[nodiscard]] bool auditionStateIsIdle() const override
    {
        return !auditionDefinitionId_.has_value()
            && !auditionButton_.getToggleState()
            && auditionButton_.getButtonText() == "Audition"
            && auditionButton_.getTitle() == "Audition reusable Sound";
    }

    [[nodiscard]] bool hasAccessibleWorkflowControls() const override
    {
        return searchInput_.getTitle().isNotEmpty()
            && searchInput_.getComponentID() == "library.sound.search"
            && scopeBox_.getTitle().isNotEmpty()
            && scopeBox_.getComponentID() == "library.sound.collection"
            && sourceBox_.getTitle().isNotEmpty()
            && sourceBox_.getComponentID() == "library.sound.source"
            && compatibilityBox_.getTitle().isNotEmpty()
            && compatibilityBox_.getComponentID()
                == "library.sound.compatibility"
            && healthBox_.getTitle().isNotEmpty()
            && healthBox_.getComponentID() == "library.sound.health"
            && categoryBox_.getTitle().isNotEmpty()
            && categoryBox_.getComponentID() == "library.sound.category"
            && originBox_.getTitle().isNotEmpty()
            && originBox_.getComponentID() == "library.sound.origin"
            && list_.getTitle().isNotEmpty()
            && list_.getComponentID() == "library.sound.results"
            && auditionInputBox_.getTitle().isNotEmpty()
            && auditionInputBox_.getComponentID()
                == "library.sound.audition.input"
            && auditionSourceBox_.getTitle().isNotEmpty()
            && auditionSourceBox_.getComponentID()
                == "library.sound.audition.source"
            && auditionValueBox_.getTitle().isNotEmpty()
            && auditionValueBox_.getComponentID()
                == "library.sound.audition.value"
            && bookmarkButton_.getTitle().isNotEmpty()
            && auditionButton_.getTitle().isNotEmpty()
            && routeButton_.getTitle().isNotEmpty()
            && parentButton_.getTitle().isNotEmpty();
    }

    void focusSearch() override
    {
        if (searchInput_.isShowing())
            searchInput_.grabKeyboardFocus();
    }

    [[nodiscard]] bool searchCanReceiveKeyboardFocus() const override
    {
        return searchInput_.isVisible()
            && searchInput_.isEnabled()
            && searchInput_.getWantsKeyboardFocus();
    }

    [[nodiscard]] bool hierarchyNavigationIsAccessibleForSmoke() const override
    {
        return showButton_.getTitle().isNotEmpty() &&
               restoreButton_.getTitle().isNotEmpty() &&
               !breadcrumbButtons_.isEmpty() &&
               std::ranges::all_of(breadcrumbButtons_,
                   [](const auto* button)
                   {
                       return button != nullptr &&
                              button->getTitle().isNotEmpty() &&
                              !button->getBounds().isEmpty() &&
                              button->getWantsKeyboardFocus();
                   });
    }

    [[nodiscard]] bool libraryProjectionIsTruthfulForSmoke() const override
    {
        const auto hasProject = std::ranges::any_of(
            libraryEntries_, [](const auto& entry) {
                return entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::ProjectDefinition;
            });
        const auto actionClosure = std::ranges::all_of(
            libraryEntries_, [](const auto& entry) {
                return entry.compatibility
                           == talas::app::SoundLibraryCompatibility::Ready
                    || !talas::app::soundLibraryHasAction(
                        entry.actions, talas::app::SoundLibraryAction::Audition);
            });
        const auto productClosure = std::ranges::all_of(
            libraryEntries_, [this](const auto& entry) {
                if (entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::OriginalExample)
                {
                    return actionAvailable(
                               entry, talas::app::SoundLibraryAction::OpenExample)
                            == talas::app::soundLibraryHasAction(
                                entry.actions,
                                talas::app::SoundLibraryAction::OpenExample)
                        && actionAvailable(
                               entry, talas::app::SoundLibraryAction::UseInProject)
                            == talas::app::soundLibraryHasAction(
                                entry.actions,
                                talas::app::SoundLibraryAction::UseInProject)
                        && actionAvailable(
                               entry, talas::app::SoundLibraryAction::OpenHelp)
                            == talas::app::soundLibraryHasAction(
                                entry.actions,
                                talas::app::SoundLibraryAction::OpenHelp)
                        && actionAvailable(
                               entry,
                               talas::app::SoundLibraryAction::Audition)
                            == talas::app::soundLibraryHasAction(
                                entry.actions,
                                talas::app::SoundLibraryAction::Audition);
                }
                return true;
            });
        return hasProject && actionClosure && productClosure
            && searchInput_.getTitle().isNotEmpty()
            && sourceBox_.getNumItems() == 4
            && compatibilityBox_.getNumItems() == 4
            && healthBox_.getNumItems() == 4
            && categoryBox_.getNumItems() > 1
            && originBox_.getNumItems() > 1;
    }

    [[nodiscard]] std::optional<std::string>
    libraryCollectionsSmokeError() override
    {
        const auto original = libraryState_;
        const auto originalSearch = searchInput_.getText();
        const auto example = std::ranges::find_if(
            libraryEntries_, [](const auto& entry) {
                return entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::OriginalExample;
            });
        if (example == libraryEntries_.end())
            return "Sound Library has no original example fixture";
        refreshing_ = true;
        searchInput_.clear();
        libraryState_.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::All;
        libraryState_.compatibilityFilter =
            talas::project::WorkspaceSoundLibraryCompatibilityFilter::All;
        libraryState_.healthFilter =
            talas::project::WorkspaceSoundLibraryHealthFilter::All;
        libraryState_.category.reset();
        libraryState_.origin.reset();
        libraryState_.favoriteEntryIds = {example->stableId};
        libraryState_.recentEntryIds = {example->stableId};
        libraryState_.collection =
            talas::project::WorkspaceSoundLibraryCollection::Favorites;
        rebuildVisibleDefinitions(example->stableId);
        const auto favoritesExact = visibleEntries_.size() == 1
            && visibleEntries_.front().stableId == example->stableId;
        libraryState_.collection =
            talas::project::WorkspaceSoundLibraryCollection::Recent;
        rebuildVisibleDefinitions(example->stableId);
        const auto recentExact = visibleEntries_.size() == 1
            && visibleEntries_.front().stableId == example->stableId;
        libraryState_.collection =
            talas::project::WorkspaceSoundLibraryCollection::All;
        libraryState_.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::OriginalExamples;
        rebuildVisibleDefinitions(example->stableId);
        const auto sourceExact = !visibleEntries_.empty()
            && std::ranges::all_of(visibleEntries_, [](const auto& entry) {
                return entry.sourceKind
                    == talas::app::SoundLibrarySourceKind::OriginalExample;
            });
        libraryState_.selectedEntryId = example->stableId;
        libraryState_.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::ProjectDefinitions;
        rebuildVisibleDefinitions();
        const auto temporarilyHidden = selectedEntryId().empty()
            && libraryState_.selectedEntryId
                == std::optional<std::string>{example->stableId};
        libraryState_.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::All;
        rebuildVisibleDefinitions();
        const auto selectionRestored = selectedEntryId() == example->stableId;
        libraryState_ = original;
        searchInput_.setText(originalSearch, false);
        refreshing_ = false;
        setLibraryState(original);
        if (!favoritesExact)
            return "Sound Library Favorites projection was not exact";
        if (!recentExact)
            return "Sound Library Recent projection was not exact";
        if (!sourceExact)
            return "Sound Library original-example filter was not exact";
        if (!temporarilyHidden)
            return "Sound Library hidden selection was not preserved";
        if (!selectionRestored)
            return "Sound Library selection was not restored after filtering";
        return std::nullopt;
    }

    [[nodiscard]] bool auditionInputSubstitutionsWorkForSmoke() override
    {
        const auto original = libraryState_;
        const auto candidate = std::ranges::find_if(
            libraryEntries_, [](const auto& entry) {
                return talas::app::soundLibraryHasAction(
                           entry.actions,
                           talas::app::SoundLibraryAction::Audition)
                    && std::ranges::any_of(
                        entry.auditionInputs,
                        [](const auto& input) {
                            return input.type == talas::core::PortType::Audio;
                        });
            });
        if (candidate == libraryEntries_.end())
            return false;
        auto smokeState = original;
        smokeState.selectedEntryId = candidate->stableId;
        smokeState.collection =
            talas::project::WorkspaceSoundLibraryCollection::All;
        smokeState.sourceFilter =
            talas::project::WorkspaceSoundLibrarySourceFilter::All;
        smokeState.compatibilityFilter =
            talas::project::WorkspaceSoundLibraryCompatibilityFilter::All;
        smokeState.healthFilter =
            talas::project::WorkspaceSoundLibraryHealthFilter::All;
        smokeState.category.reset();
        smokeState.origin.reset();
        smokeState.inputSubstitutions.clear();
        setLibraryState(smokeState);

        const auto audioInput = std::ranges::find(
            candidate->auditionInputs,
            talas::core::PortType::Audio,
            &talas::project::ProjectSoundPort::type);
        const auto inputIndex = static_cast<int>(std::distance(
            candidate->auditionInputs.begin(), audioInput));
        auditionInputBox_.setSelectedId(
            inputIndex + 1, juce::sendNotificationSync);
        auditionSourceBox_.setSelectedId(3, juce::sendNotificationSync);
        auditionValueBox_.setText("220", juce::sendNotificationSync);
        const auto authored = std::ranges::find(
            libraryState_.inputSubstitutions,
            audioInput->portId,
            &talas::project::WorkspaceSoundLibraryInputSubstitution::portId);
        const auto exactSine =
            authored != libraryState_.inputSubstitutions.end()
            && authored->source
                == talas::project::WorkspaceSoundLibraryAuditionSource::Sine
            && authored->value == 220.0
            && !authored->frozenAssetId.has_value();

        auditionSourceBox_.setSelectedId(1, juce::sendNotificationSync);
        const auto defaultRemoved = std::ranges::none_of(
            libraryState_.inputSubstitutions,
            [&](const auto& substitution) {
                return substitution.portId == audioInput->portId;
            });
        setLibraryState(original);
        publishLibraryState();
        return exactSine && defaultRemoved;
    }

    void triggerAuditionActionForSmoke() override
    {
        if (auditionButton_.onClick)
            auditionButton_.onClick();
    }

    [[nodiscard]] bool auditionActionIsSwitchForSmoke() const override
    {
        return auditionButton_.isEnabled()
            && auditionButton_.getToggleState()
            && auditionButton_.getButtonText() == "Switch Preview"
            && auditionButton_.getTitle()
                == "Switch isolated Sound audition";
    }

    [[nodiscard]] bool libraryLayoutModesWorkForSmoke() override
    {
        const auto originalWidth = getWidth();
        const auto originalHeight = getHeight();
        setSize(std::max(220, originalWidth), 390);
        const auto expanded = !compactLayout_ && detailLabel_.isVisible()
            && auditionInputBox_.isVisible()
            && auditionSourceBox_.isVisible()
            && auditionValueBox_.isVisible()
            && createButton_.isVisible() && renameButton_.isVisible()
            && deleteButton_.isVisible() && extractButton_.isVisible()
            && inlineButton_.isVisible() && parentButton_.isVisible()
            && bookmarkButton_.isVisible() && restoreButton_.isVisible();
        setSize(std::max(220, originalWidth), 238);
        const auto compact = compactLayout_ && !detailLabel_.isVisible()
            && !auditionInputBox_.isVisible()
            && !auditionSourceBox_.isVisible()
            && !auditionValueBox_.isVisible()
            && !createButton_.isVisible() && !renameButton_.isVisible()
            && !deleteButton_.isVisible() && !extractButton_.isVisible()
            && !inlineButton_.isVisible() && !parentButton_.isVisible()
            && !bookmarkButton_.isVisible() && !restoreButton_.isVisible();
        setSize(originalWidth, originalHeight);
        return expanded && compact;
    }

    void refresh(const talas::project::ProjectDocument& document) override
    {
        refreshing_ = true;
        allDefinitions_ = talas::app::soundDefinitionBrowserEntries(document);
        std::erase_if(bookmarks_,
            [&](const auto& definitionId)
            {
                return std::ranges::none_of(allDefinitions_,
                    [&](const auto& definition)
                    { return definition.definitionId == definitionId; });
            });
        refreshing_ = false;
        rebuildVisibleDefinitions(libraryState_.selectedEntryId.value_or(""));
        rebuildBreadcrumbButtons();
    }

    void setLibraryEntries(
        std::vector<talas::app::SoundLibraryEntry> entries,
        std::string diagnostic) override
    {
        const auto retained = selectedEntryId();
        libraryEntries_ = std::move(entries);
        libraryDiagnostic_ = std::move(diagnostic);
        const auto retainedCategory = libraryState_.category;
        const auto retainedOrigin = libraryState_.origin;
        std::vector<std::string> categories;
        std::vector<std::string> origins;
        for (const auto& entry : libraryEntries_)
        {
            if (!entry.category.empty())
                categories.push_back(entry.category);
            if (!entry.origin.empty())
                origins.push_back(entry.origin);
        }
        std::ranges::sort(categories);
        categories.erase(std::unique(categories.begin(), categories.end()), categories.end());
        // The demos' category leads the list, where a new user looks first.
        std::ranges::stable_partition(categories, [](const auto& category) {
            return category == talas::app::soundLibraryDemoCategory;
        });
        std::ranges::sort(origins);
        origins.erase(std::unique(origins.begin(), origins.end()), origins.end());
        refreshing_ = true;
        categoryBox_.clear(juce::dontSendNotification);
        categoryBox_.addItem("All categories", 1);
        for (const auto& category : categories)
            categoryBox_.addItem(juceString(category), categoryBox_.getNumItems() + 1);
        originBox_.clear(juce::dontSendNotification);
        originBox_.addItem("All origins", 1);
        for (const auto& origin : origins)
            originBox_.addItem(juceString(origin), originBox_.getNumItems() + 1);
        categoryBox_.setText(
            retainedCategory.has_value() ? juceString(*retainedCategory) : "All categories",
            juce::dontSendNotification);
        originBox_.setText(
            retainedOrigin.has_value() ? juceString(*retainedOrigin) : "All origins",
            juce::dontSendNotification);
        refreshing_ = false;
        std::vector<std::string> validIds;
        validIds.reserve(libraryEntries_.size());
        for (const auto& entry : libraryEntries_)
            validIds.push_back(entry.stableId);
        talas::project::reconcileWorkspaceSoundLibraryState(
            libraryState_, validIds);
        rebuildVisibleDefinitions(retained);
    }

    void setLibraryState(
        const talas::project::WorkspaceSoundLibraryState& state) override
    {
        refreshing_ = true;
        libraryState_ = state;
        scopeBox_.setSelectedId(
            static_cast<int>(libraryState_.collection) + 1,
            juce::dontSendNotification);
        sourceBox_.setSelectedId(
            static_cast<int>(libraryState_.sourceFilter) + 1,
            juce::dontSendNotification);
        compatibilityBox_.setSelectedId(
            static_cast<int>(libraryState_.compatibilityFilter) + 1,
            juce::dontSendNotification);
        healthBox_.setSelectedId(
            static_cast<int>(libraryState_.healthFilter) + 1,
            juce::dontSendNotification);
        categoryBox_.setText(
            libraryState_.category.has_value()
                ? juceString(*libraryState_.category)
                : "All categories",
            juce::dontSendNotification);
        originBox_.setText(
            libraryState_.origin.has_value()
                ? juceString(*libraryState_.origin)
                : "All origins",
            juce::dontSendNotification);
        refreshing_ = false;
        rebuildVisibleDefinitions(
            libraryState_.selectedEntryId.value_or(""));
    }

    void setProductActionSupport(
        SoundLibraryProductActionSupport support) override
    {
        productActionSupport_ = support;
        updateSelectionState();
        rebuildAuditionInputControls();
    }

    [[nodiscard]] talas::project::WorkspaceSoundLibraryState
    libraryState() const override
    {
        return libraryState_;
    }

    [[nodiscard]] std::string selectedEntryId() const override
    {
        const auto row = list_.getSelectedRow();
        if (row < 0 || row >= static_cast<int>(visibleEntries_.size()))
            return {};
        return visibleEntries_[static_cast<std::size_t>(row)].stableId;
    }

    // A project Sound drags into the graph or the Timeline; a component
    // drags as its prototype. Library examples are used in the project
    // first.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& selectedRows) override
    {
        if (selectedRows.size() != 1)
            return {};
        const auto row = selectedRows[0];
        if (row < 0 || row >= static_cast<int>(visibleEntries_.size()))
            return {};
        const auto& entry = visibleEntries_[static_cast<std::size_t>(row)];
        if (entry.sourceKind == talas::app::SoundLibrarySourceKind::ProjectDefinition
            && entry.definitionId.has_value())
        {
            return juceString(soundDragPrefix) + juceString(*entry.definitionId);
        }
        if (entry.sourceKind == talas::app::SoundLibrarySourceKind::Component
            && entry.descriptorTypeId.has_value())
        {
            return juceString(prototypeDragPrefix) + juceString(*entry.descriptorTypeId);
        }
        return {};
    }

    bool mayDragToExternalWindows() const override
    {
        return false;
    }

    [[nodiscard]] juce::String dragDescriptionForSmoke(std::string_view stableId) override
    {
        const auto found = std::ranges::find(
            visibleEntries_, stableId, &talas::app::SoundLibraryEntry::stableId);
        if (found == visibleEntries_.end())
            return {};
        juce::SparseSet<int> rows;
        rows.addRange({static_cast<int>(found - visibleEntries_.begin()),
            static_cast<int>(found - visibleEntries_.begin()) + 1});
        return getDragSourceDescription(rows).toString();
    }

    [[nodiscard]] std::vector<std::string> visibleEntryIdsForSmoke() const override
    {
        std::vector<std::string> ids;
        ids.reserve(visibleEntries_.size());
        for (const auto& entry : visibleEntries_)
            ids.push_back(entry.stableId);
        return ids;
    }

    [[nodiscard]] std::vector<std::string> categoryChoicesForSmoke() const override
    {
        std::vector<std::string> choices;
        for (int index = 0; index < categoryBox_.getNumItems(); ++index)
            choices.push_back(categoryBox_.getItemText(index).toStdString());
        return choices;
    }

    [[nodiscard]] juce::String rowTextForSmoke(std::string_view stableId) const override
    {
        const auto found = std::ranges::find(
            visibleEntries_, stableId, &talas::app::SoundLibraryEntry::stableId);
        return found == visibleEntries_.end()
            ? juce::String{}
            : juceString(found->displayName) + "\n" + entryDetailLine(*found);
    }

    [[nodiscard]] juce::String detailTextForSmoke() const override
    {
        return detailLabel_.getText();
    }

    [[nodiscard]] bool auditionEnabledForSmoke() const override
    {
        return auditionButton_.isEnabled();
    }

    [[nodiscard]] std::string selectedDefinitionId() const override
    {
        const auto row = list_.getSelectedRow();
        if (row < 0 || row >= static_cast<int>(visibleEntries_.size()))
            return {};
        return visibleEntries_[static_cast<std::size_t>(row)]
            .definitionId.value_or("");
    }

    void paint(juce::Graphics& graphics) override
    {
        WorkbenchStyle::paintPanel(graphics, getLocalBounds().toFloat(), WorkbenchStyle::panelRadius());
        WorkbenchStyle::paintPanelTitle(graphics, getLocalBounds().reduced(10).removeFromTop(20), "Sound Library");
    }

    void resized() override
    {
        compactLayout_ = getHeight() < 380;
        for (auto* control : {&createButton_, &renameButton_, &deleteButton_,
                 &extractButton_, &inlineButton_, &parentButton_,
                 &bookmarkButton_, &restoreButton_})
        {
            control->setVisible(!compactLayout_);
        }
        for (auto* control : {
                 &auditionInputBox_,
                 &auditionSourceBox_,
                 &auditionValueBox_})
        {
            control->setVisible(!compactLayout_);
        }
        auto bounds = getLocalBounds().reduced(6);
        bounds.removeFromTop(24);
        auto searchRow = bounds.removeFromTop(24);
        bookmarkButton_.setBounds(
            searchRow.removeFromRight(86).reduced(1));
        scopeBox_.setBounds(
            searchRow.removeFromRight(72).reduced(1));
        searchInput_.setBounds(searchRow.reduced(1));
        bounds.removeFromTop(4);
        auto filterRow = bounds.removeFromTop(24);
        const auto filterWidth = filterRow.getWidth() / 3;
        sourceBox_.setBounds(filterRow.removeFromLeft(filterWidth).reduced(1));
        categoryBox_.setBounds(filterRow.removeFromLeft(filterWidth).reduced(1));
        originBox_.setBounds(filterRow.reduced(1));
        auto stateFilterRow = bounds.removeFromTop(24);
        compatibilityBox_.setBounds(
            stateFilterRow.removeFromLeft(stateFilterRow.getWidth() / 2).reduced(1));
        healthBox_.setBounds(stateFilterRow.reduced(1));
        bounds.removeFromTop(4);
        auto breadcrumbRow = bounds.removeFromTop(compactLayout_ ? 28 : 34);
        breadcrumbViewport_.setBounds(breadcrumbRow);
        const auto breadcrumbWidth =
            breadcrumbButtons_.isEmpty()
                ? 0
                : std::max(72,
                      breadcrumbViewport_.getWidth() /
                          breadcrumbButtons_.size());
        const auto contentWidth = std::max(
            breadcrumbViewport_.getWidth(),
            breadcrumbWidth * breadcrumbButtons_.size());
        breadcrumbContainer_.setSize(
            contentWidth,
            std::max(1, breadcrumbViewport_.getHeight() - 8));
        auto buttonBounds = breadcrumbContainer_.getLocalBounds();
        for (auto* button : breadcrumbButtons_)
        {
            button->setBounds(
                buttonBounds.removeFromLeft(breadcrumbWidth).reduced(1));
        }
        if (compactLayout_)
        {
            detailLabel_.setVisible(false);
            list_.setBounds(bounds.removeFromTop(
                std::max(34, bounds.getHeight() - 56)));
            auto primaryRow = bounds.removeFromTop(26);
            const auto primaryWidth = primaryRow.getWidth() / 4;
            favoriteButton_.setBounds(
                primaryRow.removeFromLeft(primaryWidth).reduced(1));
            addButton_.setBounds(
                primaryRow.removeFromLeft(primaryWidth).reduced(1));
            auditionButton_.setBounds(
                primaryRow.removeFromLeft(primaryWidth).reduced(1));
            routeButton_.setBounds(primaryRow.reduced(1));
            auto secondaryRow = bounds.removeFromTop(26);
            openExampleButton_.setBounds(
                secondaryRow.removeFromLeft(secondaryRow.getWidth() / 2).reduced(1));
            showButton_.setBounds(openExampleButton_.getBounds());
            helpButton_.setBounds(secondaryRow.reduced(1));
            updateSelectionState();
            return;
        }
        detailLabel_.setVisible(true);
        list_.setBounds(bounds.removeFromTop(
            std::max(34, bounds.getHeight() - 218)));
        detailLabel_.setBounds(bounds.removeFromTop(48).reduced(2));
        bounds.removeFromTop(4);
        auto auditionInputRow = bounds.removeFromTop(26);
        const auto auditionInputWidth = auditionInputRow.getWidth() / 3;
        auditionInputBox_.setBounds(
            auditionInputRow.removeFromLeft(auditionInputWidth).reduced(1));
        auditionSourceBox_.setBounds(
            auditionInputRow.removeFromLeft(auditionInputWidth).reduced(1));
        auditionValueBox_.setBounds(auditionInputRow.reduced(1));
        auto firstRow = bounds.removeFromTop(26);
        const auto firstWidth = firstRow.getWidth();
        createButton_.setBounds(
            firstRow.removeFromLeft(firstWidth * 24 / 100).reduced(1));
        renameButton_.setBounds(
            firstRow.removeFromLeft(firstWidth * 31 / 100).reduced(1));
        deleteButton_.setBounds(firstRow.reduced(1));
        auto secondRow = bounds.removeFromTop(26);
        const auto secondWidth = secondRow.getWidth() / 4;
        favoriteButton_.setBounds(
            secondRow.removeFromLeft(secondWidth).reduced(1));
        addButton_.setBounds(
            secondRow.removeFromLeft(secondWidth).reduced(1));
        auditionButton_.setBounds(
            secondRow.removeFromLeft(secondWidth).reduced(1));
        routeButton_.setBounds(secondRow.reduced(1));
        auto thirdRow = bounds.removeFromTop(26);
        const auto thirdWidth = thirdRow.getWidth() / 3;
        openExampleButton_.setBounds(
            thirdRow.removeFromLeft(thirdWidth).reduced(1));
        helpButton_.setBounds(
            thirdRow.removeFromLeft(thirdWidth).reduced(1));
        showButton_.setBounds(thirdRow.reduced(1));
        auto fourthRow = bounds.removeFromTop(26);
        const auto fourthWidth = fourthRow.getWidth() / 3;
        parentButton_.setBounds(
            fourthRow.removeFromLeft(fourthWidth).reduced(1));
        bookmarkButton_.setBounds(
            fourthRow.removeFromLeft(fourthWidth).reduced(1));
        restoreButton_.setBounds(fourthRow.reduced(1));
        auto fifthRow = bounds.removeFromTop(26);
        extractButton_.setBounds(
            fifthRow.removeFromLeft(fifthRow.getWidth() / 2).reduced(1));
        inlineButton_.setBounds(fifthRow.reduced(1));
        updateSelectionState();
    }

  private:
    int getNumRows() override { return static_cast<int>(visibleEntries_.size()); }

    juce::String getNameForRow(int row) override
    {
        if (row < 0 || row >= static_cast<int>(visibleEntries_.size()))
            return {};
        const auto& entry = visibleEntries_[static_cast<std::size_t>(row)];
        return juceString(entry.displayName)
            + ", " + entryDetailLine(entry).replace(juceString("  ·  "), ", ")
            + ", stable id " + juceString(entry.stableId)
            + (std::ranges::find(libraryState_.favoriteEntryIds, entry.stableId)
                    != libraryState_.favoriteEntryIds.end()
                   ? ", favorite"
                   : juce::String{});
    }

    void paintListBoxItem(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected) override
    {
        if (row < 0
            || row >= static_cast<int>(visibleEntries_.size()))
        {
            return;
        }
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRoundedRectangle(juce::Rectangle<float>{2.0F, 1.0F, static_cast<float>(width) - 4.0F, static_cast<float>(height) - 2.0F}, 5.0F);
        }
        const auto& entry = visibleEntries_[static_cast<std::size_t>(row)];
        graphics.setColour(textColour);
        graphics.setFont(
            WorkbenchType::bodyStrong());
        graphics.drawText(
            juceString(entry.displayName),
            8,
            2,
            width - 16,
            16,
            juce::Justification::centredLeft);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            entryDetailLine(entry)
                + (std::ranges::find(libraryState_.favoriteEntryIds, entry.stableId)
                        != libraryState_.favoriteEntryIds.end()
                    ? juceString("  ·  FAVORITE")
                    : juce::String{}),
            8,
            17,
            width - 16,
            14,
            juce::Justification::centredLeft);
    }

    void selectedRowsChanged(int) override
    {
        if (!refreshing_)
        {
            const auto selected = selectedEntryId();
            if (!selected.empty())
            {
                if (libraryState_.selectedEntryId
                    != std::optional<std::string>{selected})
                {
                    libraryState_.inputSubstitutions.clear();
                    auditionInputDiagnostic_.clear();
                }
                libraryState_.selectedEntryId = selected;
                std::erase(libraryState_.recentEntryIds, selected);
                libraryState_.recentEntryIds.insert(
                    libraryState_.recentEntryIds.begin(), selected);
                if (libraryState_.recentEntryIds.size()
                    > talas::project::maximumWorkspaceSoundLibraryRecents)
                {
                    libraryState_.recentEntryIds.pop_back();
                }
                publishLibraryState();
            }
            else
                libraryState_.selectedEntryId.reset();
        }
        updateSelectionState();
        rebuildAuditionInputControls();
    }

    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < static_cast<int>(visibleEntries_.size()))
            performPrimaryAction(visibleEntries_[static_cast<std::size_t>(row)]);
    }

    void returnKeyPressed(int row) override
    {
        if (row >= 0 && row < static_cast<int>(visibleEntries_.size()))
            performPrimaryAction(visibleEntries_[static_cast<std::size_t>(row)]);
    }

    void rebuildVisibleDefinitions(std::string retained = {})
    {
        if (retained.empty())
            retained = selectedEntryId();
        if (retained.empty() && libraryState_.selectedEntryId.has_value())
            retained = *libraryState_.selectedEntryId;
        visibleEntries_.clear();
        const auto query = searchInput_.getText().toStdString();
        const auto containsId = [](const auto& ids, std::string_view id) {
            return std::ranges::find(ids, id) != ids.end();
        };
        const auto sourceMatches = [&](const auto& entry) {
            switch (libraryState_.sourceFilter)
            {
            case talas::project::WorkspaceSoundLibrarySourceFilter::All:
                return true;
            case talas::project::WorkspaceSoundLibrarySourceFilter::ProjectDefinitions:
                return entry.sourceKind == talas::app::SoundLibrarySourceKind::ProjectDefinition;
            case talas::project::WorkspaceSoundLibrarySourceFilter::Components:
                return entry.sourceKind == talas::app::SoundLibrarySourceKind::Component;
            case talas::project::WorkspaceSoundLibrarySourceFilter::OriginalExamples:
                return entry.sourceKind == talas::app::SoundLibrarySourceKind::OriginalExample;
            }
            return false;
        };
        const auto compatibilityMatches = [&](const auto& entry) {
            switch (libraryState_.compatibilityFilter)
            {
            case talas::project::WorkspaceSoundLibraryCompatibilityFilter::All:
                return true;
            case talas::project::WorkspaceSoundLibraryCompatibilityFilter::Ready:
                return entry.compatibility == talas::app::SoundLibraryCompatibility::Ready;
            case talas::project::WorkspaceSoundLibraryCompatibilityFilter::NeedsAttention:
                return entry.compatibility == talas::app::SoundLibraryCompatibility::ValidationOnly
                    || entry.compatibility == talas::app::SoundLibraryCompatibility::MissingDependency
                    || entry.compatibility == talas::app::SoundLibraryCompatibility::MissingAsset
                    || entry.compatibility == talas::app::SoundLibraryCompatibility::UnsupportedRuntime;
            case talas::project::WorkspaceSoundLibraryCompatibilityFilter::Unavailable:
                return entry.compatibility == talas::app::SoundLibraryCompatibility::PreservedUnavailable
                    || entry.compatibility == talas::app::SoundLibraryCompatibility::Malformed;
            }
            return false;
        };
        const auto healthMatches = [&](const auto& entry) {
            const auto dependency = entry.dependencyHealth;
            const auto asset = entry.assetHealth;
            switch (libraryState_.healthFilter)
            {
            case talas::project::WorkspaceSoundLibraryHealthFilter::All:
                return true;
            case talas::project::WorkspaceSoundLibraryHealthFilter::Healthy:
                return dependency == talas::app::SoundLibraryHealth::Healthy
                    && asset == talas::app::SoundLibraryHealth::Healthy;
            case talas::project::WorkspaceSoundLibraryHealthFilter::Warning:
                return dependency == talas::app::SoundLibraryHealth::Warning
                    || asset == talas::app::SoundLibraryHealth::Warning;
            case talas::project::WorkspaceSoundLibraryHealthFilter::Unavailable:
                return dependency == talas::app::SoundLibraryHealth::Unavailable
                    || dependency == talas::app::SoundLibraryHealth::Invalid
                    || asset == talas::app::SoundLibraryHealth::Unavailable
                    || asset == talas::app::SoundLibraryHealth::Invalid;
            }
            return false;
        };
        for (const auto& entry : libraryEntries_)
        {
            const auto collectionMatches =
                libraryState_.collection == talas::project::WorkspaceSoundLibraryCollection::All
                || (libraryState_.collection == talas::project::WorkspaceSoundLibraryCollection::Favorites
                    && containsId(libraryState_.favoriteEntryIds, entry.stableId))
                || (libraryState_.collection == talas::project::WorkspaceSoundLibraryCollection::Recent
                    && containsId(libraryState_.recentEntryIds, entry.stableId));
            if (collectionMatches && sourceMatches(entry)
                && compatibilityMatches(entry) && healthMatches(entry)
                && (!libraryState_.category.has_value()
                    || entry.category == *libraryState_.category)
                && (!libraryState_.origin.has_value()
                    || entry.origin == *libraryState_.origin)
                && talas::app::soundLibraryEntryMatches(entry, query))
                visibleEntries_.push_back(entry);
        }
        if (libraryState_.collection
            == talas::project::WorkspaceSoundLibraryCollection::Recent)
        {
            std::ranges::stable_sort(
                visibleEntries_,
                [&](const auto& left, const auto& right) {
                    return std::ranges::find(
                               libraryState_.recentEntryIds,
                               left.stableId)
                        < std::ranges::find(
                              libraryState_.recentEntryIds,
                              right.stableId);
                });
        }
        refreshing_ = true;
        list_.updateContent();
        const auto found = std::ranges::find_if(
            visibleEntries_,
            [&](const auto& entry) { return entry.stableId == retained; });
        if (found == visibleEntries_.end())
            list_.deselectAllRows();
        else
            list_.selectRow(static_cast<int>(
                std::distance(visibleEntries_.begin(), found)));
        refreshing_ = false;
        updateSelectionState();
        repaint();
    }

    void rebuildBreadcrumbButtons()
    {
        breadcrumbButtons_.clear(true);
        const auto add = [this](std::string definitionId, juce::String label)
        {
            auto* button =
                breadcrumbButtons_.add(new juce::TextButton(std::move(label)));
            button->setComponentID(
                definitionId.empty()
                    ? "library.sound.breadcrumb.root"
                    : "library.sound.breadcrumb." + juceString(definitionId));
            breadcrumbContainer_.addAndMakeVisible(button);
            describeControl(*button,
                definitionId.empty()
                    ? juce::String{"Show Project Root in Structure"}
                    : "Show " + button->getButtonText() + " in Structure",
                "Navigates to this hierarchy scope while retaining "
                "per-scope selection and graph view.");
            button->onClick = [this, definitionId = std::move(definitionId)]
            {
                juce::Component::SafePointer<SoundDefinitionsPanelImpl> safe{
                    this};
                juce::MessageManager::callAsync(
                    [safe, definitionId]
                    {
                        if (safe != nullptr)
                        {
                            safe->onAction_(Action::NavigateBreadcrumb,
                                definitionId);
                        }
                    });
            };
        };
        add({}, "Root");
        for (const auto& definitionId : navigationPath_)
        {
            const auto found = std::ranges::find(allDefinitions_,
                definitionId,
                &talas::app::SoundDefinitionBrowserEntry::definitionId);
            add(definitionId,
                found == allDefinitions_.end()
                    ? juceString(definitionId)
                    : juceString(found->displayName));
        }
        resized();
        breadcrumbViewport_.setViewPosition(
            std::max(0,
                breadcrumbContainer_.getWidth() -
                    breadcrumbViewport_.getWidth()),
            0);
    }

    [[nodiscard]] const talas::project::ProjectSoundPort*
    selectedAuditionInput() const noexcept
    {
        const auto* entry = selectedEntry();
        const auto index = auditionInputBox_.getSelectedId() - 1;
        if (entry == nullptr || index < 0
            || static_cast<std::size_t>(index)
                >= entry->auditionInputs.size())
        {
            return nullptr;
        }
        return &entry->auditionInputs[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] const talas::project::WorkspaceSoundLibraryInputSubstitution*
    substitutionFor(std::string_view portId) const noexcept
    {
        const auto found = std::ranges::find(
            libraryState_.inputSubstitutions,
            portId,
            &talas::project::WorkspaceSoundLibraryInputSubstitution::portId);
        return found == libraryState_.inputSubstitutions.end()
            ? nullptr
            : &*found;
    }

    void rebuildAuditionInputControls()
    {
        const auto retainedPort = selectedAuditionInput() == nullptr
            ? std::string{}
            : selectedAuditionInput()->portId;
        const auto wasRefreshing = refreshing_;
        refreshing_ = true;
        auditionInputBox_.clear(juce::dontSendNotification);
        const auto* entry = selectedEntry();
        if (entry == nullptr || entry->auditionInputs.empty())
        {
            auditionInputBox_.addItem("No replaceable inputs", 1);
            auditionInputBox_.setSelectedId(1, juce::dontSendNotification);
            auditionInputBox_.setEnabled(false);
            auditionSourceBox_.clear(juce::dontSendNotification);
            auditionSourceBox_.addItem("Default", 1);
            auditionSourceBox_.setSelectedId(1, juce::dontSendNotification);
            auditionSourceBox_.setEnabled(false);
            auditionValueBox_.clear(juce::dontSendNotification);
            auditionValueBox_.addItem("No value", 1);
            auditionValueBox_.setSelectedId(1, juce::dontSendNotification);
            auditionValueBox_.setEnabled(false);
            refreshing_ = wasRefreshing;
            return;
        }
        auto selectedIndex = std::size_t{};
        for (std::size_t index = 0; index < entry->auditionInputs.size(); ++index)
        {
            const auto& input = entry->auditionInputs[index];
            auditionInputBox_.addItem(
                juceString(input.portId) + juceString(" · ")
                    + juceString(auditionPortTypeLabel(input.type)),
                static_cast<int>(index + 1));
            if ((!retainedPort.empty() && input.portId == retainedPort)
                || (retainedPort.empty()
                    && substitutionFor(input.portId) != nullptr))
            {
                selectedIndex = index;
            }
        }
        auditionInputBox_.setSelectedId(
            static_cast<int>(selectedIndex + 1),
            juce::dontSendNotification);
        auditionInputBox_.setEnabled(true);
        refreshing_ = wasRefreshing;
        rebuildAuditionSourceControls();
    }

    void rebuildAuditionSourceControls()
    {
        const auto wasRefreshing = refreshing_;
        refreshing_ = true;
        auditionSourceBox_.clear(juce::dontSendNotification);
        const auto* entry = selectedEntry();
        const auto* input = selectedAuditionInput();
        if (entry == nullptr || input == nullptr)
        {
            auditionSourceBox_.addItem("Default", 1);
            auditionSourceBox_.setSelectedId(1, juce::dontSendNotification);
            auditionSourceBox_.setEnabled(false);
            refreshing_ = wasRefreshing;
            rebuildAuditionValueControls();
            return;
        }
        if (!actionAvailable(
                *entry, talas::app::SoundLibraryAction::Audition))
        {
            auditionSourceBox_.addItem("Audition unavailable", 1);
            auditionSourceBox_.setSelectedId(1, juce::dontSendNotification);
            auditionSourceBox_.setEnabled(false);
            refreshing_ = wasRefreshing;
            rebuildAuditionValueControls();
            return;
        }

        auditionSourceBox_.addItem("Default", 1);
        auto selectedId = 1;
        const auto* substitution = substitutionFor(input->portId);
        if (input->type == talas::core::PortType::Audio)
        {
            auditionSourceBox_.addItem("Silence", 2);
            auditionSourceBox_.addItem("Sine", 3);
            if (!entry->auditionAudioAssetIds.empty())
                auditionSourceBox_.addItem("Frozen asset", 4);
            if (substitution != nullptr)
            {
                switch (substitution->source)
                {
                case talas::project::WorkspaceSoundLibraryAuditionSource::Silence:
                    selectedId = 2;
                    break;
                case talas::project::WorkspaceSoundLibraryAuditionSource::Sine:
                    selectedId = 3;
                    break;
                case talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset:
                    selectedId = entry->auditionAudioAssetIds.empty() ? 1 : 4;
                    break;
                default:
                    break;
                }
            }
            auditionSourceBox_.setEnabled(true);
        }
        else if (input->type == talas::core::PortType::Control)
        {
            auditionSourceBox_.addItem("Constant", 2);
            if (substitution != nullptr
                && substitution->source
                    == talas::project::WorkspaceSoundLibraryAuditionSource::Constant)
            {
                selectedId = 2;
            }
            auditionSourceBox_.setEnabled(true);
        }
        else
        {
            auditionSourceBox_.clear(juce::dontSendNotification);
            auditionSourceBox_.addItem("Unsupported input type", 1);
            auditionSourceBox_.setEnabled(false);
        }
        auditionSourceBox_.setSelectedId(
            selectedId, juce::dontSendNotification);
        refreshing_ = wasRefreshing;
        rebuildAuditionValueControls();
    }

    void rebuildAuditionValueControls()
    {
        const auto wasRefreshing = refreshing_;
        refreshing_ = true;
        auditionValueBox_.clear(juce::dontSendNotification);
        auditionValueBox_.setEditableText(false);
        const auto* entry = selectedEntry();
        const auto* input = selectedAuditionInput();
        const auto sourceId = auditionSourceBox_.getSelectedId();
        const auto* substitution = input == nullptr
            ? nullptr
            : substitutionFor(input->portId);
        if (entry != nullptr && input != nullptr
            && input->type == talas::core::PortType::Audio && sourceId == 3)
        {
            auditionValueBox_.setEditableText(true);
            for (const auto frequency : {110, 220, 440, 880})
                auditionValueBox_.addItem(juce::String(frequency), frequency);
            const auto value = substitution != nullptr
                    && substitution->source
                        == talas::project::WorkspaceSoundLibraryAuditionSource::Sine
                ? substitution->value
                : 440.0;
            auditionValueBox_.setText(
                juce::String(value, 3), juce::dontSendNotification);
            auditionValueBox_.setEnabled(true);
        }
        else if (entry != nullptr && input != nullptr
                 && input->type == talas::core::PortType::Audio && sourceId == 4)
        {
            for (std::size_t index = 0;
                 index < entry->auditionAudioAssetIds.size();
                 ++index)
            {
                auditionValueBox_.addItem(
                    juceString(entry->auditionAudioAssetIds[index]),
                    static_cast<int>(index + 1));
            }
            auto selectedId = 1;
            if (substitution != nullptr
                && substitution->frozenAssetId.has_value())
            {
                const auto found = std::ranges::find(
                    entry->auditionAudioAssetIds,
                    *substitution->frozenAssetId);
                if (found != entry->auditionAudioAssetIds.end())
                {
                    selectedId = static_cast<int>(
                        std::distance(
                            entry->auditionAudioAssetIds.begin(), found)
                        + 1);
                }
            }
            auditionValueBox_.setSelectedId(
                selectedId, juce::dontSendNotification);
            auditionValueBox_.setEnabled(
                !entry->auditionAudioAssetIds.empty());
        }
        else if (input != nullptr
                 && input->type == talas::core::PortType::Control
                 && sourceId == 2)
        {
            auditionValueBox_.setEditableText(true);
            for (const auto value : {-1.0, 0.0, 0.5, 1.0})
            {
                auditionValueBox_.addItem(
                    juce::String(value, 1),
                    auditionValueBox_.getNumItems() + 1);
            }
            const auto value = substitution != nullptr
                    && substitution->source
                        == talas::project::WorkspaceSoundLibraryAuditionSource::Constant
                ? substitution->value
                : 0.5;
            auditionValueBox_.setText(
                juce::String(value, 3), juce::dontSendNotification);
            auditionValueBox_.setEnabled(true);
        }
        else
        {
            auditionValueBox_.addItem("No value", 1);
            auditionValueBox_.setSelectedId(1, juce::dontSendNotification);
            auditionValueBox_.setEnabled(false);
        }
        refreshing_ = wasRefreshing;
    }

    void commitAuditionInputSubstitution()
    {
        const auto* entry = selectedEntry();
        const auto* input = selectedAuditionInput();
        if (entry == nullptr || input == nullptr
            || !actionAvailable(
                *entry, talas::app::SoundLibraryAction::Audition))
            return;
        const auto sourceId = auditionSourceBox_.getSelectedId();
        talas::project::WorkspaceSoundLibraryInputSubstitution candidate;
        candidate.portId = input->portId;
        bool retain = sourceId > 1;
        if (input->type == talas::core::PortType::Audio && sourceId == 2)
        {
            candidate.source =
                talas::project::WorkspaceSoundLibraryAuditionSource::Silence;
        }
        else if (input->type == talas::core::PortType::Audio && sourceId == 3)
        {
            const auto value = parseFiniteNumber(
                auditionValueBox_.getText().toStdString());
            if (!value.has_value()
                || *value < talas::project::minimumWorkspaceSoundLibrarySineFrequencyHz
                || *value > talas::project::maximumWorkspaceSoundLibrarySineFrequencyHz)
            {
                auditionInputDiagnostic_ =
                    "Sine frequency must remain inside the closed audition range.";
                updateSelectionState();
                return;
            }
            candidate.source =
                talas::project::WorkspaceSoundLibraryAuditionSource::Sine;
            candidate.value = *value;
        }
        else if (input->type == talas::core::PortType::Audio && sourceId == 4)
        {
            const auto assetId = auditionValueBox_.getText().toStdString();
            if (std::ranges::find(entry->auditionAudioAssetIds, assetId)
                == entry->auditionAudioAssetIds.end())
            {
                auditionInputDiagnostic_ =
                    "Frozen audition requires one verified Audio asset.";
                updateSelectionState();
                return;
            }
            candidate.source =
                talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset;
            candidate.frozenAssetId = assetId;
        }
        else if (input->type == talas::core::PortType::Control && sourceId == 2)
        {
            const auto value = parseFiniteNumber(
                auditionValueBox_.getText().toStdString());
            if (!value.has_value()
                || std::abs(*value)
                    > talas::project::maximumWorkspaceSoundLibraryConstantMagnitude)
            {
                auditionInputDiagnostic_ =
                    "Control constant exceeds the closed audition range.";
                updateSelectionState();
                return;
            }
            candidate.source =
                talas::project::WorkspaceSoundLibraryAuditionSource::Constant;
            candidate.value = *value;
        }
        else
        {
            retain = false;
        }

        std::erase_if(
            libraryState_.inputSubstitutions,
            [&](const auto& substitution) {
                return substitution.portId == input->portId;
            });
        if (retain)
            libraryState_.inputSubstitutions.push_back(std::move(candidate));
        std::ranges::sort(
            libraryState_.inputSubstitutions,
            {},
            &talas::project::WorkspaceSoundLibraryInputSubstitution::portId);
        auditionInputDiagnostic_.clear();
        updateSelectionState();
        publishLibraryState();
    }

    void updateSelectionState()
    {
        const auto selected = selectedEntryId();
        const auto* entry = selectedEntry();
        const auto hasSelection = entry != nullptr;
        const auto selectedAuditionable = hasSelection
            && actionAvailable(*entry, talas::app::SoundLibraryAction::Audition);
        const auto switching = selectedAuditionable
            && auditionDefinitionId_.has_value()
            && selected != *auditionDefinitionId_;
        const auto active = auditionDefinitionId_.has_value();
        auditionButton_.setEnabled(
            (selectedAuditionable || active)
            && !controlsCurrentProject_);
        const auto routableSelection = selectedAuditionable
            && entry->sourceKind
                == talas::app::SoundLibrarySourceKind::OriginalExample
            && std::ranges::find(entry->tags, "controller-route")
                != entry->tags.end();
        routeButton_.setEnabled(routableSelection
            || controlsCurrentProject_);
        routeButton_.setToggleState(
            controlsCurrentProject_, juce::dontSendNotification);
        routeButton_.setButtonText(
            controlsCurrentProject_ ? "Stop" : "Control");
        describeControl(routeButton_,
            controlsCurrentProject_ ? "Stop controlling current project"
                : "Control current project",
            controlsCurrentProject_
                ? "Stops routing generated Event Values and restores the prior project transport state."
                : "Runs a selected controller example and routes its generated Event Value to a matching prepared VCS control in the current project.");
        auditionButton_.setToggleState(
            active && !controlsCurrentProject_,
            juce::dontSendNotification);
        if (controlsCurrentProject_)
        {
            auditionButton_.setButtonText("Audition");
            describeControl(auditionButton_,
                "Private audition unavailable during project control",
                "Stop the controller route before starting a private preview.");
        }
        else if (switching)
        {
            auditionButton_.setButtonText("Switch Preview");
            describeControl(auditionButton_,
                "Switch isolated Sound audition",
                "Stops " + juceString(*auditionDefinitionId_)
                    + " and stages the selected exact reusable Sound "
                      "without changing project topology.");
        }
        else if (active)
        {
            auditionButton_.setButtonText("Stop Preview");
            describeControl(auditionButton_,
                "Stop isolated Sound audition",
                "Stops the isolated preview of "
                    + juceString(*auditionDefinitionId_)
                    + " and returns transport control to the project graph.");
        }
        else
        {
            auditionButton_.setButtonText("Audition");
            describeControl(auditionButton_,
                "Audition reusable Sound",
                "Stages an isolated prepared preview with bounded local "
                "input substitutes without changing project topology.");
        }
        const auto projectDefinition = hasSelection
            && entry->sourceKind
                == talas::app::SoundLibrarySourceKind::ProjectDefinition;
        renameButton_.setEnabled(projectDefinition);
        deleteButton_.setEnabled(projectDefinition);
        addButton_.setEnabled(hasSelection
            && actionAvailable(
                *entry, talas::app::SoundLibraryAction::UseInProject));
        bookmarkButton_.setEnabled(projectDefinition);
        bookmarkButton_.setButtonText(
            projectDefinition
                && bookmarks_.contains(entry->definitionId.value_or(""))
                ? "Remove View"
                : "Bookmark View");
        restoreButton_.setEnabled(
            projectDefinition
            && bookmarks_.contains(entry->definitionId.value_or("")));
        showButton_.setEnabled(hasSelection
            && actionAvailable(
                *entry, talas::app::SoundLibraryAction::ShowDefinition));
        favoriteButton_.setEnabled(hasSelection);
        favoriteButton_.setButtonText(
            hasSelection
                && std::ranges::find(
                    libraryState_.favoriteEntryIds, selected)
                    != libraryState_.favoriteEntryIds.end()
                ? "Unfavorite"
                : "Favorite");
        openExampleButton_.setEnabled(hasSelection
            && actionAvailable(
                *entry, talas::app::SoundLibraryAction::OpenExample));
        helpButton_.setEnabled(hasSelection
            && actionAvailable(
                *entry, talas::app::SoundLibraryAction::OpenHelp));
        parentButton_.setEnabled(!navigationPath_.empty());
        openExampleButton_.setVisible(!compactLayout_ || (hasSelection
            && entry->sourceKind
                == talas::app::SoundLibrarySourceKind::OriginalExample));
        showButton_.setVisible(!compactLayout_ || (hasSelection
            && entry->sourceKind
                == talas::app::SoundLibrarySourceKind::ProjectDefinition));
        if (entry == nullptr)
        {
            detailLabel_.setText(
                libraryDiagnostic_.empty()
                    ? "Select an entry to inspect exact compatibility and actions."
                    : juceString(libraryDiagnostic_),
                juce::dontSendNotification);
        }
        else
        {
            const auto guidance = demoGuidance(*entry);
            detailLabel_.setText(
                juceString(entry->summary) + "\n"
                    + (guidance.isEmpty() ? juce::String{}
                                          : guidance + juceString(" · "))
                    + juceString(compatibilityLabel(entry->compatibility))
                    + (entry->actionDiagnostic.empty()
                        ? juce::String{}
                        : juceString(" · ")
                            + juceString(entry->actionDiagnostic))
                    + (auditionInputDiagnostic_.isEmpty()
                        ? juce::String{}
                        : juceString(" · ") + auditionInputDiagnostic_),
                juce::dontSendNotification);
        }
    }

    [[nodiscard]] const talas::app::SoundLibraryEntry* selectedEntry() const
    {
        const auto row = list_.getSelectedRow();
        if (row < 0 || row >= static_cast<int>(visibleEntries_.size()))
            return nullptr;
        return &visibleEntries_[static_cast<std::size_t>(row)];
    }

    void performPrimaryAction(const talas::app::SoundLibraryEntry& entry)
    {
        if (actionAvailable(
                entry, talas::app::SoundLibraryAction::ShowDefinition))
        {
            onAction_(Action::OpenDefinition,
                entry.definitionId.value_or(""));
        }
        else if (actionAvailable(
                     entry, talas::app::SoundLibraryAction::OpenExample))
        {
            onAction_(Action::OpenExample, entry.stableId);
        }
        else if (actionAvailable(
                     entry, talas::app::SoundLibraryAction::OpenHelp))
        {
            onAction_(Action::OpenHelp, entry.stableId);
        }
    }

    [[nodiscard]] talas::app::SoundLibraryActionMask productActionsFor(
        talas::app::SoundLibrarySourceKind source) const noexcept
    {
        switch (source)
        {
        case talas::app::SoundLibrarySourceKind::ProjectDefinition:
            return productActionSupport_.projectDefinitions;
        case talas::app::SoundLibrarySourceKind::Component:
            return productActionSupport_.components;
        case talas::app::SoundLibrarySourceKind::OriginalExample:
            return productActionSupport_.originalExamples;
        }
        return 0;
    }

    [[nodiscard]] bool actionAvailable(
        const talas::app::SoundLibraryEntry& entry,
        talas::app::SoundLibraryAction action) const noexcept
    {
        return talas::app::soundLibraryHasAction(entry.actions, action)
            && talas::app::soundLibraryHasAction(
                productActionsFor(entry.sourceKind), action);
    }

    void publishLibraryState(
        Action action = Action::LibraryStateChanged)
    {
        if (!refreshing_)
            onAction_(action, selectedEntryId());
    }

    ActionCallback onAction_;
    std::vector<talas::app::SoundDefinitionBrowserEntry> allDefinitions_;
    std::vector<talas::app::SoundLibraryEntry> libraryEntries_;
    std::vector<talas::app::SoundLibraryEntry> visibleEntries_;
    talas::project::WorkspaceSoundLibraryState libraryState_;
    std::string libraryDiagnostic_;
    std::unordered_set<std::string> bookmarks_;
    std::vector<std::string> navigationPath_;
    std::optional<std::string> auditionDefinitionId_;
    bool controlsCurrentProject_ = false;
    SoundLibraryProductActionSupport productActionSupport_;
    juce::TextEditor searchInput_;
    juce::ComboBox scopeBox_;
    juce::ComboBox sourceBox_;
    juce::ComboBox compatibilityBox_;
    juce::ComboBox healthBox_;
    juce::ComboBox categoryBox_;
    juce::ComboBox originBox_;
    juce::ListBox list_;
    juce::Label detailLabel_;
    juce::ComboBox auditionInputBox_;
    juce::ComboBox auditionSourceBox_;
    juce::ComboBox auditionValueBox_;
    juce::String auditionInputDiagnostic_;
    juce::Component breadcrumbContainer_;
    juce::Viewport breadcrumbViewport_;
    juce::OwnedArray<juce::TextButton> breadcrumbButtons_;
    juce::TextButton createButton_;
    juce::TextButton renameButton_;
    juce::TextButton deleteButton_;
    juce::TextButton addButton_;
    juce::TextButton extractButton_;
    juce::TextButton inlineButton_;
    juce::TextButton parentButton_;
    juce::TextButton auditionButton_;
    juce::TextButton routeButton_;
    juce::TextButton bookmarkButton_;
    juce::TextButton restoreButton_;
    juce::TextButton showButton_;
    juce::TextButton favoriteButton_;
    juce::TextButton openExampleButton_;
    juce::TextButton helpButton_;
    bool refreshing_ = false;
    bool compactLayout_ = false;
};

}

std::unique_ptr<SoundDefinitionsPanel> makeSoundDefinitionsPanel(
    SoundDefinitionsPanel::ActionCallback onAction)
{
    return std::make_unique<SoundDefinitionsPanelImpl>(std::move(onAction));
}
}
