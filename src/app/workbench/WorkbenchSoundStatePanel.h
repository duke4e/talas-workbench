#pragma once

#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string_view>

namespace talas::app::workbench
{
class SoundStatePanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using LiveMutationCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    ~SoundStatePanel() override = default;

    virtual void refresh() = 0;
    virtual void refreshLiveValues() = 0;
    virtual void focusSoundState() = 0;
    [[nodiscard]] virtual bool selectDeclarationForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) = 0;
    [[nodiscard]] virtual bool selectPreparedValueForSmoke(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t voiceGeneration) = 0;
    virtual void createDeclarationForSmoke(
        talas::project::ProjectSoundVariableDeclaration declaration) = 0;
    [[nodiscard]] virtual bool assignSelectionForSmoke(
        std::string_view definitionId) = 0;
    [[nodiscard]] virtual bool resetSelectionForSmoke() = 0;
    [[nodiscard]] virtual bool captureSelectionForSmoke() = 0;
    [[nodiscard]] virtual bool surfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view selectedDefinitionId,
        bool assignmentEnabled) const = 0;
};

[[nodiscard]] std::unique_ptr<SoundStatePanel> makeSoundStatePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    SoundStatePanel::DocumentChangeCallback onDocumentChange,
    SoundStatePanel::LiveMutationCallback onLiveMutation,
    SoundStatePanel::StatusCallback onStatus);
}
