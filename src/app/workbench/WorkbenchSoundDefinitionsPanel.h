#pragma once

#include "talas/app/SoundLibraryModel.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/WorkspaceSessionState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
struct SoundLibraryProductActionSupport
{
    talas::app::SoundLibraryActionMask projectDefinitions = 0;
    talas::app::SoundLibraryActionMask components = 0;
    talas::app::SoundLibraryActionMask originalExamples = 0;
};

class SoundDefinitionsPanel : public juce::Component
{
public:
    enum class Action
    {
        Create,
        Rename,
        Delete,
        AddInstance,
        ExtractSelection,
        InlineSelection,
        OpenDefinition,
        ShowRoot,
        OpenParent,
        Audition,
        ControlCurrentProject,
        ToggleBookmark,
        RestoreBookmark,
        NavigateBreadcrumb,
        OpenExample,
        OpenHelp,
        UseInProject,
        ToggleFavorite,
        LibraryStateChanged,
    };

    using ActionCallback = std::function<void(Action, std::string)>;
    ~SoundDefinitionsPanel() override = default;

    virtual void showRootContext() = 0;
    virtual void setNavigationPath(std::vector<std::string> path) = 0;
    virtual void setBookmarks(
        const std::vector<std::string>& definitionIds) = 0;
    virtual void selectDefinition(std::string_view definitionId) = 0;
    virtual void setAuditionState(
        std::optional<std::string> definitionId,
        bool controlsCurrentProject = false) = 0;
    [[nodiscard]] virtual bool hasActiveAuditionState(
        std::string_view definitionId) const = 0;
    [[nodiscard]] virtual bool auditionStateIsIdle() const = 0;
    [[nodiscard]] virtual bool hasAccessibleWorkflowControls() const = 0;
    virtual void focusSearch() = 0;
    [[nodiscard]] virtual bool searchCanReceiveKeyboardFocus() const = 0;
    [[nodiscard]] virtual bool
    hierarchyNavigationIsAccessibleForSmoke() const = 0;
    [[nodiscard]] virtual bool libraryProjectionIsTruthfulForSmoke() const = 0;
    [[nodiscard]] virtual std::optional<std::string>
    libraryCollectionsSmokeError() = 0;
    [[nodiscard]] virtual bool
    auditionInputSubstitutionsWorkForSmoke() = 0;
    [[nodiscard]] virtual bool auditionActionIsSwitchForSmoke() const = 0;
    virtual void triggerAuditionActionForSmoke() = 0;
    [[nodiscard]] virtual bool controllerRouteAvailableForSmoke() const = 0;
    virtual void triggerControllerRouteActionForSmoke() = 0;
    [[nodiscard]] virtual bool libraryLayoutModesWorkForSmoke() = 0;
    virtual void refresh(const talas::project::ProjectDocument& document) = 0;
    virtual void setLibraryEntries(
        std::vector<talas::app::SoundLibraryEntry> entries,
        std::string diagnostic) = 0;
    virtual void setLibraryState(
        const talas::project::WorkspaceSoundLibraryState& state) = 0;
    virtual void setProductActionSupport(
        SoundLibraryProductActionSupport support) = 0;
    [[nodiscard]] virtual talas::project::WorkspaceSoundLibraryState
    libraryState() const = 0;
    [[nodiscard]] virtual std::string selectedEntryId() const = 0;
    [[nodiscard]] virtual std::string selectedDefinitionId() const = 0;
    // What dragging the entry carries, empty when it does not drag.
    [[nodiscard]] virtual juce::String dragDescriptionForSmoke(
        std::string_view stableId) = 0;
};

[[nodiscard]] std::unique_ptr<SoundDefinitionsPanel> makeSoundDefinitionsPanel(
    SoundDefinitionsPanel::ActionCallback onAction);
} // namespace talas::app::workbench
