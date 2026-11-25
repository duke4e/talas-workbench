#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
// The Class Editor: a Sound definition's
// Class Description, its icon and, for each of its parameters, its name,
// type, range, Field Rectangle and comment; Apply keeps them in the
// project.
class ClassEditorPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    ~ClassEditorPanel() override = default;

    virtual void refresh() = 0;
    [[nodiscard]] virtual bool selectDefinitionForSmoke(std::string_view definitionId) = 0;
    // The class being edited, and the one to edit (Action > Edit Class).
    [[nodiscard]] virtual std::string selectedDefinition() const = 0;
    [[nodiscard]] virtual bool selectParameterForSmoke(std::string_view parameterId) = 0;
    // name, type, minimum, maximum, field, comment, description, expand
    // ("true" or "false"), or fieldLayout (the fields the Field Layout
    // draws, the highlighted one with a *, one overlapping another with a !).
    [[nodiscard]] virtual bool setFieldForSmoke(std::string_view field, const juce::String& text) = 0;
    [[nodiscard]] virtual juce::String fieldForSmoke(std::string_view field) const = 0;
    [[nodiscard]] virtual bool applyForSmoke() = 0;
    // Closing: whether the fields
    // hold changes not yet kept by Apply, kept now, or dropped.
    [[nodiscard]] virtual bool hasChangesNotKept() const = 0;
    virtual bool applyChanges() = 0;
    // The icon editor: a
    // click or a stroke on the large or small icon, another Sound's drag
    // description dropped on it, and the PNG icon box.
    [[nodiscard]] virtual talas::project::ProjectSoundIcon iconForSmoke() const = 0;
    virtual void strokeIconForSmoke(bool large, std::size_t fromX, std::size_t fromY, std::size_t toX, std::size_t toY) = 0;
    [[nodiscard]] virtual bool dropOnIconForSmoke(const juce::String& description) = 0;
    // Another Sound's drag description dropped on the Parameter list: the
    // parameters take its namesakes' types.
    [[nodiscard]] virtual bool dropOnParametersForSmoke(const juce::String& description) = 0;
    virtual void setPngIconForSmoke(bool png) = 0;
    // The input and output types (empty for the ports' own) and the
    // Nonaudio side effects box.
    virtual void setIconTypesForSmoke(const std::string& input, const std::string& output, bool sideEffects) = 0;
    // The numbers of input and output channels, 0 for none chosen.
    virtual void setIconChannelsForSmoke(unsigned input, unsigned output) = 0;
    // The Class Description's field, which the Edit menu edits.
    [[nodiscard]] virtual juce::TextEditor& descriptionFieldForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<ClassEditorPanel> makeClassEditorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    ClassEditorPanel::DocumentChangeCallback onDocumentChange,
    ClassEditorPanel::StatusCallback onStatus);
}
