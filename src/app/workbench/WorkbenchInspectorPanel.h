#pragma once

#include "app/workbench/WorkbenchComponentReplacementPanel.h"
#include "app/workbench/WorkbenchDiskSplicerField.h"
#include "talas/project/ProjectGenerators.h"

#include "talas/core/NodeRegistry.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <string_view>

namespace talas::app::workbench
{
enum class PreservedNodeAction
{
    Retry,
    Replace,
    ConfirmReplacement,
    CancelReplacement,
    Disconnect,
    Delete,
};

struct DiskCacheCapturePresentation
{
    bool visible = false;
    bool enabled = false;
    bool cancelling = false;
    std::string sourcePath;
    juce::String label;
    juce::String detail;
};

class ParameterFieldEditor;

class InspectorPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using LiveMutationCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using BrowseResourceCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        std::string)>;
    using PreservedNodeActionCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        PreservedNodeAction)>;
    using ExpressionLanguageUpgradeCallback = std::function<void()>;
    using DiskCacheCaptureProjectionCallback = std::function<
        DiskCacheCapturePresentation(
            std::optional<std::string>, std::string_view)>;
    using DiskCacheCaptureActionCallback = std::function<void(
        std::optional<std::string>, std::string)>;

    ~InspectorPanel() override = default;

    virtual void setSelectedNodeId(std::string nodeId) = 0;
    virtual void setSoundDefinitionContext(
        std::optional<std::string> definitionId) = 0;
    virtual void focusParameter(std::string parameterId) = 0;
    [[nodiscard]] virtual bool focusedParameterForSmoke(
        std::string_view parameterId) const = 0;
    // Smoke access: characters typed into the value field one at a time,
    // over all of it first or not; the field's text after, and the part
    // selected.
    [[nodiscard]] virtual std::pair<std::string, std::string> typeIntoValueForSmoke(std::string_view, bool = false)
    {
        return {};
    }
    // A key pressed in the value field; its text after, and the part
    // selected.
    [[nodiscard]] virtual std::pair<std::string, std::string> pressValueKeyForSmoke(const juce::KeyPress&)
    {
        return {};
    }
    // A Sound copied in the graph and pasted into the value field: the handler
    // gives its name, or none for a text.
    virtual void setSoundPasteHandler(std::function<std::optional<std::string>()>) {}
    // Paste special into an input field: the
    // handler shares the Sound copied in the graph into the selected Sound's
    // input, true when it did; and the Edit menu's Paste special into the
    // focused input field, false when no input field has the focus.
    virtual void setInputPasteSpecialHandler(std::function<bool(const std::string&, const std::string&)>) {}
    virtual bool pasteSpecialIntoFocusedInput() { return false; }
    // A click on the class name: the selected
    // Sound's class described in a help window.
    virtual void setDescribeSoundHandler(std::function<void(const talas::core::NodeTypeDescriptor&)>) {}
    // A file name typed into a file name field:
    // the handler locates and binds the file, true when it did.
    using ResourceLocator = std::function<bool(const std::optional<std::string>& definitionId,
        const std::string& nodeId, const std::string& parameterId, const std::string& name)>;
    virtual void setResourceLocator(ResourceLocator) {}
    // A Sound of a signal flow dragged into a field: the handler gives
    // that Sound's value for the field's parameter.
    using DraggedSoundValue = std::function<std::optional<std::string>(const std::string& description,
        const std::string& parameterId)>;
    virtual void setDraggedSoundValue(DraggedSoundValue) {}
    // A prototype or a signal flow's Sound dropped into an input field
    //: the handler adds it as that input's last.
    using SoundDroppedIntoInput = std::function<bool(const std::string& description, const std::string& nodeId,
        const std::string& portId)>;
    virtual void setSoundDroppedIntoInput(SoundDroppedIntoInput) {}
    // Tab past the Sound's last field: the signal
    // flow diagram is next.
    virtual void setLeaveFieldsHandler(std::function<void()>) {}
    // Smoke access: a file's name dropped from the File Organizer on the
    // selected field; whether the field took it.
    [[nodiscard]] virtual bool dropFileForSmoke(const std::filesystem::path&) { return false; }
    // How a file field shows the file it names:
    // its whole path or its name alone, none for a resource not a file.
    using FileNameDisplay = std::function<std::optional<std::string>(const std::string& resourceId)>;
    virtual void setFileNameDisplay(FileNameDisplay) {}
    // Icon Size in Sound Editor Fields: the
    // Sounds of input fields as large icons, else small ones.
    virtual void setLargeFieldIcons(bool) {}
    // A field holds an illegal value it refused.
    [[nodiscard]] virtual bool holdsIllegalValue() const { return false; }
    // The problem dropped, the field showing its value again.
    virtual void dropIllegalValue() {}
    // What was typed into the value field and not yet taken
    // is tested now, and taken when legal; false while the field holds an
    // illegal value.
    virtual bool testTypedValue() { return !holdsIllegalValue(); }
    // Smoke access: a text typed into the value field (none to type
    // nothing), then Tab or Shift+Tab; the field selected after, and
    // whether all of its text is selected.
    [[nodiscard]] virtual std::pair<std::string, bool> tabFromValueForSmoke(std::string_view, bool) { return {}; }
    // A double-click in the background of the parameter fields: they take
    // over the window, or give it back.
    virtual void setBackgroundDoubleClick(std::function<void()>) {}
    // Smoke access: the tooltips over the class name and the parameter's
    // name, and a click on the class name.
    [[nodiscard]] virtual std::pair<juce::String, juce::String> helpTooltipsForSmoke() { return {}; }
    // The class name at the lower right of the parameters and whether it
    // lies there; a click on it.
    [[nodiscard]] virtual std::pair<juce::String, bool> classNameCornerForSmoke() { return {}; }
    virtual void clickClassNameCornerForSmoke() {}
    // Smoke access: whether the parameter's name shows in italics, as a hot
    // field's does.
    [[nodiscard]] virtual bool parameterNameItalicForSmoke() { return false; }
    virtual void clickClassNameForSmoke() {}
    // Smoke access: a radioButtons field's buttons, `name@column,row` with
    // a * on the one chosen, and a click on one.
    [[nodiscard]] virtual juce::String radioChoicesForSmoke() const { return {}; }
    [[nodiscard]] virtual bool clickRadioChoiceForSmoke(const juce::String&) { return false; }
    // Smoke access: Command+V in the value field over all of a text, and
    // Paste special of a text copied from it into another; the field's text
    // after.
    [[nodiscard]] virtual std::string pasteIntoValueForSmoke(std::string_view) { return {}; }
    // Smoke access: a text typed into the value field and committed; how
    // many illegal values the field has signalled after.
    [[nodiscard]] virtual std::size_t commitValueForSmoke(std::string_view) { return 0; }
    [[nodiscard]] virtual std::string pasteSpecialForSmoke(std::string_view, std::string_view) { return {}; }
    // Smoke access: the value field set to a text and double-clicked at a
    // character; the text it selects.
    [[nodiscard]] virtual std::string doubleClickValueForSmoke(std::string_view, int)
    {
        return {};
    }
    virtual void setPreparedContract(juce::String contract) = 0;
    virtual void setComponentReplacementEnabled(bool enabled) = 0;
    virtual void setComponentReplacementPresentation(
        const ComponentReplacementPresentation& presentation) = 0;
    [[nodiscard]] virtual bool preparedContractContainsForSmoke(
        const juce::String& text) const = 0;
    [[nodiscard]] virtual bool showsPreservedStateForSmoke(
        const juce::String& status,
        bool parameterEditingExpected) const = 0;
    [[nodiscard]] virtual bool preservedActionsAvailableForSmoke() const = 0;
    [[nodiscard]] virtual bool componentReplacementActionAvailableForSmoke()
        const = 0;
    [[nodiscard]] virtual bool componentReplacementSurfaceReadyForSmoke(
        const juce::String& expectedText,
        bool confirmationExpected) const = 0;
    [[nodiscard]] virtual bool parameterHintContainsForSmoke(
        const juce::String& expected) const = 0;
    [[nodiscard]] virtual bool expressionLanguageReadyForSmoke(
        const juce::String& expected,
        bool upgradeVisible,
        bool upgradeEnabled,
        const juce::String& expectedUpgradeDetail = {}) const = 0;
    [[nodiscard]] virtual bool timedExpressionUpgradeReadyForSmoke(
        bool visible, bool enabled) const = 0;
    [[nodiscard]] virtual bool diskCacheCaptureReadyForSmoke(
        const juce::String& label,
        bool enabled,
        const juce::String& detail) const = 0;
    virtual void activateDiskCacheCaptureForSmoke() = 0;
    // The Inputs field: what it lists, and an edit of it applied.
    [[nodiscard]] virtual juce::String inputFieldTextForSmoke() = 0;
    [[nodiscard]] virtual bool applyInputFieldForSmoke(const juce::String& text) = 0;
    // A drop from the Expressions Library on the
    // value field of the selected expression parameter.
    [[nodiscard]] virtual bool dropExpressionForSmoke(
        const juce::String& description) = 0;
    // A prototype dragged into the selected field.
    [[nodiscard]] virtual bool dropSoundForSmoke(const juce::String& description) = 0;
    // The graphical envelope or array editor, while it shows.
    [[nodiscard]] virtual ParameterFieldEditor*
    fieldEditorForSmoke() noexcept = 0;
    // A DiskSplicer's Segments field: the
    // project's files, playing and the file list it works with, and the
    // field while it shows.
    virtual void setDiskSplicerHost(DiskSplicerField::Host) {}
    // Zoom in or out on the field's graphic editor while it has the keyboard
    // focus; false when none does.
    virtual bool zoomActiveField(bool) { return false; }
    [[nodiscard]] virtual DiskSplicerField* diskSplicerFieldForSmoke() noexcept { return nullptr; }
    [[nodiscard]] virtual juce::String parameterValueTextForSmoke() const = 0;
    [[nodiscard]] virtual bool selectParameterForSmoke(
        std::string_view parameterId) = 0;
    // The Preset's Set to Current Event Values action, when the selected
    // node is a Preset.
    [[nodiscard]] virtual bool capturePresetEventValuesForSmoke() = 0;
    virtual void activateExpressionUpgradeForSmoke() = 0;
    virtual void commitExpressionForSmoke(
        std::string nodeId,
        std::string parameterId,
        std::string source) = 0;
    virtual void commitExpressionDefinitionForSmoke(
        std::string name,
        std::string parameters,
        std::string source) = 0;
    [[nodiscard]] virtual bool expressionDefinitionReadyForSmoke(
        std::string_view name,
        std::string_view source,
        bool deleteEnabled) const = 0;
    virtual void deleteExpressionDefinitionForSmoke() = 0;
    [[nodiscard]] virtual juce::Component*
    expressionLanguageComponentForSmoke() noexcept = 0;
    [[nodiscard]] virtual juce::Component*
    expressionValueComponentForSmoke() noexcept = 0;
    virtual void focusExpressionLanguage() = 0;
    virtual void showNode() = 0;
    virtual void showVariables() = 0;
    virtual void showGenerators() = 0;
    virtual void createGeneratorForSmoke(
        talas::project::ProjectGeneratorProgram program) = 0;
    [[nodiscard]] virtual bool selectGeneratorForSmoke(
        std::string_view generatorId) = 0;
    [[nodiscard]] virtual bool generateGeneratorForSmoke() = 0;
    [[nodiscard]] virtual bool generateGeneratorTimelineForSmoke() = 0;
    [[nodiscard]] virtual bool generateGeneratorScoreForSmoke() = 0;
    [[nodiscard]] virtual bool generateGeneratorSoundForSmoke() = 0;
    [[nodiscard]] virtual bool generatorProjectionFinishedForSmoke() = 0;
    [[nodiscard]] virtual bool configureGeneratorProjectionForSmoke(
        std::uint64_t timelineBaseMicroseconds,
        std::uint64_t scoreBaseTick,
        std::string_view outputNamespace) = 0;
    [[nodiscard]] virtual bool configureGeneratorSoundForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view outputNamespace) = 0;
    [[nodiscard]] virtual bool generatorProjectionActionsEnabledForSmoke(
        bool timelineExpected,
        bool scoreExpected,
        bool soundExpected = false) = 0;
    [[nodiscard]] virtual bool requestGeneratorDeleteForSmoke() = 0;
    [[nodiscard]] virtual bool acceptGeneratorDeleteForSmoke() = 0;
    [[nodiscard]] virtual bool generatorSurfaceReadyForSmoke(
        std::string_view generatorId,
        std::string_view expectedPreview) const = 0;
    [[nodiscard]] virtual bool generatorSoundSurfaceReadyForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view expectedPreview) = 0;
    [[nodiscard]] virtual bool generatorGraphNodePickerReadyForSmoke() = 0;
    virtual void refreshVariableValues() = 0;
    [[nodiscard]] virtual bool variableSurfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view expectedStatus) const = 0;
    [[nodiscard]] virtual bool selectVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) = 0;
    [[nodiscard]] virtual bool selectPreparedVariableForSmoke(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t voiceGeneration) = 0;
    [[nodiscard]] virtual bool preparedVariableReadyForSmoke(
        std::string_view expectedInstance,
        std::string_view expectedStatus,
        bool assignmentEnabled,
        bool captureEnabled) const = 0;
    [[nodiscard]] virtual std::string preparedVariableDescriptionForSmoke()
        const = 0;
    virtual void createVariableForSmoke(
        talas::project::ProjectVariableDeclaration declaration) = 0;
    [[nodiscard]] virtual bool assignSelectedVariableForSmoke(double value) = 0;
    [[nodiscard]] virtual bool resetSelectedVariableForSmoke() = 0;
    [[nodiscard]] virtual bool captureSelectedVariableForSmoke() = 0;
    [[nodiscard]] virtual bool variableNumbersAreStrictForSmoke() const = 0;
    virtual void showSoundState() = 0;
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
    virtual void refresh() = 0;
};

[[nodiscard]] std::unique_ptr<InspectorPanel> makeInspectorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    InspectorPanel::DocumentChangeCallback onDocumentChange,
    InspectorPanel::LiveMutationCallback onVariableLiveMutation,
    InspectorPanel::StatusCallback onStatus,
    InspectorPanel::BrowseResourceCallback onBrowseResource,
    InspectorPanel::PreservedNodeActionCallback onPreservedNodeAction,
    InspectorPanel::ExpressionLanguageUpgradeCallback
        onExpressionLanguageUpgrade,
    InspectorPanel::ExpressionLanguageUpgradeCallback
        onTimedExpressionLanguageUpgrade,
    InspectorPanel::DiskCacheCaptureProjectionCallback
        diskCacheCaptureProjection,
    InspectorPanel::DiskCacheCaptureActionCallback
        onDiskCacheCaptureAction);
}
