#pragma once

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
class ProjectVariablesPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using LiveMutationCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    ~ProjectVariablesPanel() override = default;

    virtual void refresh() = 0;
    virtual void refreshLiveValues() = 0;
    virtual void focusVariables() = 0;
    virtual void showSoundState() = 0;
    [[nodiscard]] virtual bool surfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view expectedStatus) const = 0;
    [[nodiscard]] virtual bool selectVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) = 0;
    [[nodiscard]] virtual bool selectPreparedValueForSmoke(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t voiceGeneration) = 0;
    [[nodiscard]] virtual bool preparedValueReadyForSmoke(
        std::string_view expectedInstance,
        std::string_view expectedStatus,
        bool assignmentEnabled,
        bool captureEnabled) const = 0;
    [[nodiscard]] virtual std::string preparedValueDescriptionForSmoke()
        const = 0;
    virtual void createVariableForSmoke(
        talas::project::ProjectVariableDeclaration declaration) = 0;
    [[nodiscard]] virtual bool assignSelectedForSmoke(double value) = 0;
    [[nodiscard]] virtual bool resetSelectedForSmoke() = 0;
    [[nodiscard]] virtual bool captureSelectedForSmoke() = 0;
    [[nodiscard]] virtual bool rejectsMalformedNumbersForSmoke() const = 0;
    [[nodiscard]] virtual bool selectSoundVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) = 0;
    [[nodiscard]] virtual bool selectPreparedSoundValueForSmoke(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t voiceGeneration) = 0;
    virtual void createSoundVariableForSmoke(
        talas::project::ProjectSoundVariableDeclaration declaration) = 0;
    [[nodiscard]] virtual bool assignSelectedSoundForSmoke(
        std::string_view definitionId) = 0;
    [[nodiscard]] virtual bool resetSelectedSoundForSmoke() = 0;
    [[nodiscard]] virtual bool captureSelectedSoundForSmoke() = 0;
    [[nodiscard]] virtual bool soundStateReadyForSmoke(
        std::string_view variableId,
        std::string_view selectedDefinitionId,
        bool assignmentEnabled) const = 0;
};

[[nodiscard]] std::unique_ptr<ProjectVariablesPanel>
makeProjectVariablesPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    ProjectVariablesPanel::DocumentChangeCallback onDocumentChange,
    ProjectVariablesPanel::LiveMutationCallback onLiveMutation,
    ProjectVariablesPanel::StatusCallback onStatus);
}
