#include "app/workbench/WorkbenchProjectVariablesPanel.h"

#include "app/workbench/WorkbenchSoundStatePanel.h"
#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectVariables.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] juce::String domainText(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar: return "scalar";
    case talas::expr::ValueDomain::Hertz: return "hertz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::NoteNumber: return "noteNumber";
    case talas::expr::ValueDomain::Samples: return "samples";
    }
    return "scalar";
}

[[nodiscard]] std::optional<talas::expr::ValueDomain> selectedDomain(
    const juce::ComboBox& box)
{
    switch (box.getSelectedId())
    {
    case 1: return talas::expr::ValueDomain::Scalar;
    case 2: return talas::expr::ValueDomain::Hertz;
    case 3: return talas::expr::ValueDomain::Seconds;
    case 4: return talas::expr::ValueDomain::NoteNumber;
    case 5: return talas::expr::ValueDomain::Samples;
    default: return std::nullopt;
    }
}

[[nodiscard]] int domainId(talas::expr::ValueDomain domain) noexcept
{
    return static_cast<int>(domain) + 1;
}

[[nodiscard]] std::optional<double> parseFiniteNumber(
    const juce::String& text)
{
    const auto token = text.trim().toStdString();
    if (token.empty())
        return std::nullopt;
    double value = 0.0;
    const auto parsed = std::from_chars(
        token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{}
        || parsed.ptr != token.data() + token.size()
        || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] juce::String instancePathText(
    const talas::project::PreparedProjectVariableIdentity& identity)
{
    if (identity.ownerKind
        == talas::project::ProjectVariableOwnerKind::Project)
    {
        return "Project";
    }
    if (identity.ownerKind
        == talas::project::ProjectVariableOwnerKind::WorkspaceSession)
    {
        return "Workspace Session";
    }
    juce::String result = identity.definitionId.empty()
        ? juce::String{"Project"}
        : "Sound " + juceString(identity.definitionId);
    for (std::size_t index = 0; index < identity.instancePath.size(); ++index)
    {
        result += " / " + juceString(identity.instancePath[index]);
    }
    if (identity.ownerKind
        == talas::project::ProjectVariableOwnerKind::MidiVoice)
    {
        result += " / MIDI Voice " + juceString(identity.ownerNodeId);
        if (identity.voiceOrdinal.has_value())
            result += " / slot "
                + juce::String(static_cast<int>(*identity.voiceOrdinal) + 1);
    }
    return result;
}

[[nodiscard]] juce::String declarationOwnerText(
    const talas::project::ProjectVariableOwner& owner)
{
    switch (owner.kind)
    {
    case talas::project::ProjectVariableOwnerKind::Project:
        return "Project";
    case talas::project::ProjectVariableOwnerKind::SoundDefinition:
        return "Sound " + juceString(owner.definitionId);
    case talas::project::ProjectVariableOwnerKind::MidiVoice:
        return (owner.definitionId.empty()
                ? juce::String{"Project"}
                : "Sound " + juceString(owner.definitionId))
            + " / MIDI Voice " + juceString(owner.nodeId);
    case talas::project::ProjectVariableOwnerKind::WorkspaceSession:
        return "Workspace Session";
    }
    return "Unknown";
}

[[nodiscard]] bool declarationMatches(
    const talas::project::ProjectVariableDeclaration& declaration,
    const talas::project::PreparedProjectVariableIdentity& identity) noexcept
{
    if (declaration.variableId != identity.variableId
        || declaration.owner.kind != identity.ownerKind)
    {
        return false;
    }
    switch (declaration.owner.kind)
    {
    case talas::project::ProjectVariableOwnerKind::Project:
    case talas::project::ProjectVariableOwnerKind::WorkspaceSession:
        return identity.definitionId.empty()
            && identity.ownerNodeId.empty();
    case talas::project::ProjectVariableOwnerKind::SoundDefinition:
        return declaration.owner.definitionId == identity.definitionId;
    case talas::project::ProjectVariableOwnerKind::MidiVoice:
        return declaration.owner.definitionId == identity.definitionId
            && declaration.owner.nodeId == identity.ownerNodeId;
    }
    return false;
}

[[nodiscard]] juce::String referencePathsText(
    const talas::project::ProjectVariableReferencePaths& references)
{
    juce::String result;
    for (std::size_t index = 0; index < references.paths.size(); ++index)
    {
        if (index != 0)
            result += "; ";
        result += juceString(references.paths[index]);
    }
    return result;
}

class ProjectVariablesPanelImpl final : public ProjectVariablesPanel
{
public:
    ProjectVariablesPanelImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        DocumentChangeCallback onDocumentChange,
        LiveMutationCallback onLiveMutation,
        StatusCallback onStatus)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onLiveMutation_(std::move(onLiveMutation))
        , onStatus_(std::move(onStatus))
        , scalarModeButton_("Numeric")
        , soundModeButton_("Sound State")
        , soundStatePanel_(makeSoundStatePanel(
              bridge_,
              [this] { onDocumentChange_(); },
              [this] { onLiveMutation_(); },
              [this](const juce::String& status) { onStatus_(status); }))
        , newButton_("New")
        , saveButton_("Save")
        , deleteButton_("Delete")
        , assignButton_("Set live")
        , resetButton_("Reset")
        , resetAllButton_("Reset shared")
        , captureButton_("Capture initial")
    {
        setComponentID("inspector.variables.surface");
        describeControl(
            *this,
            "Project variables",
            "Bounded Project, reusable-Sound, Workspace Session, and exact MIDI Voice variables with closed persistence and live-slot authority.");
        title_.setText("Variables", juce::dontSendNotification);
        title_.setFont(WorkbenchType::labelStrong());
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(title_);
        addAndMakeVisible(scalarModeButton_);
        addAndMakeVisible(soundModeButton_);
        addChildComponent(*soundStatePanel_);
        scalarModeButton_.setComponentID("inspector.variables.mode.numeric");
        soundModeButton_.setComponentID("inspector.variables.mode.sound-state");
        scalarModeButton_.setClickingTogglesState(true);
        soundModeButton_.setClickingTogglesState(true);
        scalarModeButton_.setRadioGroupId(4102);
        soundModeButton_.setRadioGroupId(4102);
        scalarModeButton_.setToggleState(true, juce::dontSendNotification);
        describeControl(
            scalarModeButton_,
            "Numeric variables",
            "Edit numeric @variable declarations and their exact prepared live cells.");
        describeControl(
            soundModeButton_,
            "Sound State variables",
            "Edit exact Sound and ordered SoundCollection declarations, candidates, and prepared selections.");
        scalarModeButton_.onClick = [this] { setSoundMode(false); };
        soundModeButton_.onClick = [this] { setSoundMode(true); };

        for (auto* component : std::initializer_list<juce::Component*>{
                 &declarationBox_, &newButton_, &variableId_, &displayName_,
                 &ownerBox_, &domainBox_, &initialValue_, &snapshotEligible_,
                 &saveButton_, &deleteButton_, &instanceBox_, &liveValue_,
                 &assignValue_, &assignButton_, &resetButton_, &resetAllButton_,
                 &captureButton_, &status_})
        {
            addAndMakeVisible(*component);
        }

        declarationBox_.setComponentID("inspector.variables.list");
        newButton_.setComponentID("inspector.variables.new");
        variableId_.setComponentID("inspector.variable:new:id");
        displayName_.setComponentID("inspector.variable:new:display-name");
        ownerBox_.setComponentID("inspector.variable:new:owner");
        domainBox_.setComponentID("inspector.variable:new:domain");
        initialValue_.setComponentID("inspector.variable:new:initial");
        snapshotEligible_.setComponentID("inspector.variable:new:snapshot");
        saveButton_.setComponentID("inspector.variable:new:save");
        deleteButton_.setComponentID("inspector.variable:new:delete");
        instanceBox_.setComponentID("inspector.variable:new:instance");
        liveValue_.setComponentID("inspector.variable:new:live-status");
        assignValue_.setComponentID("inspector.variable:new:assign-value");
        assignButton_.setComponentID("inspector.variable:new:assign");
        resetButton_.setComponentID("inspector.variable:new:reset");
        resetAllButton_.setComponentID("inspector.variables.reset-all");
        captureButton_.setComponentID("inspector.variable:new:capture");
        status_.setComponentID("inspector.variable:new:status");

        variableId_.setTextToShowWhenEmpty("variableId", secondaryTextColour);
        variableId_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectVariableIdentifierBytes));
        displayName_.setTextToShowWhenEmpty("Display name", secondaryTextColour);
        displayName_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectVariableDisplayNameBytes));
        initialValue_.setTextToShowWhenEmpty("Initial value", secondaryTextColour);
        assignValue_.setTextToShowWhenEmpty("Live value", secondaryTextColour);
        snapshotEligible_.setButtonText("Snapshots");
        liveValue_.setReadOnly(true);
        liveValue_.setCaretVisible(false);
        liveValue_.setColour(
            juce::TextEditor::backgroundColourId, workbenchTheme.controlTile);
        liveValue_.setColour(
            juce::TextEditor::outlineColourId, panelBorderColour);
        liveValue_.setColour(
            juce::TextEditor::textColourId, secondaryTextColour);
        status_.setMultiLine(true);
        status_.setReadOnly(true);
        status_.setCaretVisible(false);
        status_.setFont(WorkbenchType::micro());
        status_.setColour(
            juce::TextEditor::backgroundColourId, workbenchTheme.controlTile);
        status_.setColour(
            juce::TextEditor::outlineColourId, panelBorderColour);
        status_.setColour(
            juce::TextEditor::textColourId, secondaryTextColour);

        domainBox_.addItem("scalar", 1);
        domainBox_.addItem("hertz", 2);
        domainBox_.addItem("seconds", 3);
        domainBox_.addItem("noteNumber", 4);
        domainBox_.addItem("samples", 5);
        domainBox_.setSelectedId(1, juce::dontSendNotification);

        describeControl(declarationBox_, "Variable declaration", "Select a stable @variableId and owner scope. Focus identity follows the selected declaration across layout and Undo.");
        describeControl(newButton_, "New variable", "Prepare one new bounded project variable declaration.");
        describeControl(variableId_, "Variable identifier", "ASCII expression identifier referenced as @variableId. Renaming never rewrites expression source text.");
        describeControl(displayName_, "Variable display name", "Human-readable label; the stable expression identity remains @variableId.");
        describeControl(ownerBox_, "Variable owner", "Project, Workspace Session, one exact reusable Sound definition, or one structurally qualified MIDI Voice owner. Display labels never replace stable IDs.");
        describeControl(domainBox_, "Variable value domain", "Scalar, hertz, seconds, noteNumber, or samples. Domain changes are re-audited against every reference.");
        describeControl(initialValue_, "Declared initial value", "Finite value restored by Reset and used for fresh project or device loading.");
        describeControl(snapshotEligible_, "Snapshot eligibility", "Allows this variable to participate in exact snapshot recall and morph transactions.");
        describeControl(saveButton_, "Save variable declaration", "Commits one failure-atomic Undo step. Identity changes require an explicit second activation and exact reference audit.");
        describeControl(deleteButton_, "Delete variable declaration", "Requires an explicit second activation and displays every exact reference path before deletion.");
        describeControl(instanceBox_, "Prepared variable instance", "Exact project/session cell, reusable Sound instance path, or MIDI Voice slot ordinal and generation with channel, note, and active/release state.");
        describeControl(liveValue_, "Current variable value", "Coherent value from the active render stage. Unavailable or stale publication is never shown as the declared initial value.");
        describeControl(assignValue_, "New live variable value", "Finite value assigned only to the selected exact prepared cell and stage.");
        describeControl(assignButton_, "Set live variable value", "Queues one stage-bound variable assignment. Voice assignments also require the exact active slot generation and never redirect after stealing.");
        describeControl(resetButton_, "Reset selected variable", "Restores the selected exact prepared cell to its declared initial value.");
        describeControl(
            resetAllButton_,
            "Reset shared variables",
            "Restores Project, reusable-Sound, and Workspace Session cells in one bounded transaction. MIDI Voice cells reset on exact slot allocation or through their selected live row.");
        describeControl(captureButton_, "Capture project variable initial value", "Copies one coherent Project-owned live value into its declaration as one Undo step. Sound-instance, Workspace Session, and MIDI Voice values cannot rewrite a shared declaration.");
        describeControl(status_, "Variable state status", "Owner, domain, initial and current state, modification, snapshot policy, and exact action result.");

        declarationBox_.onChange = [this] { selectDeclaration(); };
        instanceBox_.onChange = [this] { selectInstance(); };
        ownerBox_.onChange = [this] {
            const auto index = ownerBox_.getSelectedItemIndex();
            const auto voice = index >= 0
                && static_cast<std::size_t>(index) < ownerChoices_.size()
                && ownerChoices_[static_cast<std::size_t>(index)].kind
                    == talas::project::ProjectVariableOwnerKind::MidiVoice;
            if (voice)
                snapshotEligible_.setToggleState(false, juce::dontSendNotification);
            snapshotEligible_.setEnabled(!voice);
        };
        newButton_.onClick = [this] { beginNew(); };
        saveButton_.onClick = [this] { saveDeclaration(); };
        deleteButton_.onClick = [this] { deleteDeclaration(); };
        assignButton_.onClick = [this] { assignLive(); };
        resetButton_.onClick = [this] { resetSelected(); };
        resetAllButton_.onClick = [this] { resetAll(); };
        captureButton_.onClick = [this] { captureInitial(); };
        refresh();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this, juce::AccessibilityRole::group);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(4);
        auto heading = content.removeFromTop(20);
        soundModeButton_.setBounds(heading.removeFromRight(90));
        heading.removeFromRight(3);
        scalarModeButton_.setBounds(heading.removeFromRight(62));
        title_.setBounds(heading);
        content.removeFromTop(3);
        if (soundMode_)
        {
            soundStatePanel_->setBounds(content);
            return;
        }
        soundStatePanel_->setBounds({});
        auto selection = content.removeFromTop(24);
        newButton_.setBounds(selection.removeFromRight(48));
        selection.removeFromRight(4);
        declarationBox_.setBounds(selection);
        content.removeFromTop(3);
        auto identity = content.removeFromTop(24);
        ownerBox_.setBounds(identity.removeFromRight(
            std::min(126, identity.getWidth() / 2)));
        identity.removeFromRight(4);
        variableId_.setBounds(identity);
        content.removeFromTop(3);
        displayName_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        auto declaration = content.removeFromTop(24);
        snapshotEligible_.setBounds(declaration.removeFromRight(86));
        declaration.removeFromRight(4);
        domainBox_.setBounds(declaration.removeFromRight(
            std::min(96, declaration.getWidth() / 2)));
        declaration.removeFromRight(4);
        initialValue_.setBounds(declaration);
        content.removeFromTop(3);
        auto editActions = content.removeFromTop(24);
        deleteButton_.setBounds(editActions.removeFromRight(58));
        editActions.removeFromRight(4);
        saveButton_.setBounds(editActions.removeFromRight(58));
        content.removeFromTop(7);
        instanceBox_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        liveValue_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        auto assignment = content.removeFromTop(24);
        assignButton_.setBounds(assignment.removeFromRight(66));
        assignment.removeFromRight(4);
        assignValue_.setBounds(assignment);
        content.removeFromTop(3);
        auto liveActions = content.removeFromTop(24);
        const auto third = std::max(1, (liveActions.getWidth() - 8) / 3);
        resetButton_.setBounds(liveActions.removeFromLeft(third));
        liveActions.removeFromLeft(4);
        resetAllButton_.setBounds(liveActions.removeFromLeft(third));
        liveActions.removeFromLeft(4);
        captureButton_.setBounds(liveActions);
        content.removeFromTop(5);
        status_.setBounds(content);
    }

    void refresh() override
    {
        soundStatePanel_->refresh();
        pendingDestructiveIdentity_.reset();
        const auto retained = selectedDeclaration_;
        declarationChoices_.clear();
        declarationBox_.clear(juce::dontSendNotification);
        const auto& document = bridge_.editor().document();
        const auto enabled = talas::expr::supportsVariableReferences(
            document.expressionLanguage);
        for (const auto& declaration : document.variableLibrary.variables)
        {
            declarationChoices_.push_back({declaration.owner, declaration.variableId});
            declarationBox_.addItem(
                declarationOwnerText(declaration.owner)
                    + " | @" + juceString(declaration.variableId),
                static_cast<int>(declarationChoices_.size()));
        }
        newButton_.setEnabled(enabled
            && declarationChoices_.size()
                < talas::project::maximumProjectVariableCount);
        rebuildOwners();
        if (retained.has_value())
        {
            const auto found = std::ranges::find(declarationChoices_, *retained);
            if (found != declarationChoices_.end())
            {
                declarationBox_.setSelectedItemIndex(
                    static_cast<int>(std::distance(declarationChoices_.begin(), found)),
                    juce::dontSendNotification);
                loadDeclaration(*retained);
                return;
            }
        }
        if (!declarationChoices_.empty())
        {
            declarationBox_.setSelectedItemIndex(0, juce::dontSendNotification);
            loadDeclaration(declarationChoices_.front());
        }
        else
        {
            beginNew();
            status_.setText(
                enabled
                    ? "No variables declared | create a bounded @variableId"
                    : "Variables require an explicit Variables v1 project upgrade",
                false);
        }
    }

    void refreshLiveValues() override
    {
        soundStatePanel_->refreshLiveValues();
        rebuildInstances();
        updateLivePresentation();
    }

    void focusVariables() override
    {
        if (soundMode_)
        {
            soundStatePanel_->focusSoundState();
            return;
        }
        auto& target = selectedDeclaration_.has_value()
            ? static_cast<juce::Component&>(declarationBox_)
            : static_cast<juce::Component&>(newButton_);
        if (target.isShowing())
            target.grabKeyboardFocus();
    }

    void showSoundState() override
    {
        setSoundMode(true);
        soundStatePanel_->focusSoundState();
    }

    bool surfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view expectedStatus) const override
    {
        return selectedDeclaration_.has_value()
            && selectedDeclaration_->variableId == variableId
            && declarationBox_.getComponentID().contains(juceString(variableId))
            && status_.getText().contains(juceString(expectedStatus))
            && status_.isReadOnly();
    }

    bool selectVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) override
    {
        setSoundMode(false);
        const DeclarationIdentity identity{owner, std::string{variableId}};
        const auto found = std::ranges::find(declarationChoices_, identity);
        if (found == declarationChoices_.end())
            return false;
        declarationBox_.setSelectedItemIndex(
            static_cast<int>(std::distance(declarationChoices_.begin(), found)),
            juce::dontSendNotification);
        loadDeclaration(identity);
        return selectedDeclaration_ == identity;
    }

    bool selectPreparedValueForSmoke(
        const talas::project::PreparedProjectVariableIdentity& identity,
        std::uint64_t voiceGeneration) override
    {
        refreshLiveValues();
        const PreparedChoice choice{identity, voiceGeneration};
        const auto found = std::ranges::find(preparedChoices_, choice);
        if (found == preparedChoices_.end())
            return false;
        instanceBox_.setSelectedItemIndex(
            static_cast<int>(std::distance(preparedChoices_.begin(), found)),
            juce::dontSendNotification);
        selectedPreparedChoice_ = choice;
        updatePreparedFocusIdentity();
        updateLivePresentation();
        return true;
    }

    bool preparedValueReadyForSmoke(
        std::string_view expectedInstance,
        std::string_view expectedStatus,
        bool assignmentEnabled,
        bool captureEnabled) const override
    {
        return selectedPreparedChoice_.has_value()
            && instanceBox_.getText().contains(juceString(expectedInstance))
            && status_.getText().contains(juceString(expectedStatus))
            && assignButton_.isEnabled() == assignmentEnabled
            && resetButton_.isEnabled() == assignmentEnabled
            && captureButton_.isEnabled() == captureEnabled
            && instanceBox_.getComponentID().contains(
                juceString(selectedPreparedChoice_->identity.variableId));
    }

    std::string preparedValueDescriptionForSmoke() const override
    {
        return instanceBox_.getText().toStdString()
            + " || " + status_.getText().toStdString()
            + " || assign=" + (assignButton_.isEnabled() ? "1" : "0")
            + " reset=" + (resetButton_.isEnabled() ? "1" : "0")
            + " capture=" + (captureButton_.isEnabled() ? "1" : "0")
            + " id=" + instanceBox_.getComponentID().toStdString();
    }

    void createVariableForSmoke(
        talas::project::ProjectVariableDeclaration declaration) override
    {
        setSoundMode(false);
        const auto result = bridge_.editor().upsertProjectVariable(declaration);
        if (result.ok())
        {
            selectedDeclaration_ = DeclarationIdentity{
                declaration.owner, declaration.variableId};
            onDocumentChange_();
        }
        else
        {
            onStatus_("Variable create blocked: " + juceString(result.message));
        }
        refresh();
    }

    bool assignSelectedForSmoke(double value) override
    {
        assignValue_.setText(juce::String(value, 9), false);
        return assignLive();
    }

    bool resetSelectedForSmoke() override { return resetSelected(); }
    bool captureSelectedForSmoke() override { return captureInitial(); }
    bool rejectsMalformedNumbersForSmoke() const override
    {
        return !parseFiniteNumber("").has_value()
            && !parseFiniteNumber("+").has_value()
            && !parseFiniteNumber("e").has_value()
            && !parseFiniteNumber("1e").has_value()
            && parseFiniteNumber("-1.25e2")
                == std::optional<double>{-125.0};
    }

    bool selectSoundVariableForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) override
    {
        setSoundMode(true);
        return soundStatePanel_->selectDeclarationForSmoke(owner, variableId);
    }

    bool selectPreparedSoundValueForSmoke(
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
        std::uint64_t voiceGeneration) override
    {
        setSoundMode(true);
        return soundStatePanel_->selectPreparedValueForSmoke(
            identity,
            voiceGeneration);
    }

    void createSoundVariableForSmoke(
        talas::project::ProjectSoundVariableDeclaration declaration) override
    {
        setSoundMode(true);
        soundStatePanel_->createDeclarationForSmoke(std::move(declaration));
    }

    bool assignSelectedSoundForSmoke(
        std::string_view definitionId) override
    {
        return soundStatePanel_->assignSelectionForSmoke(definitionId);
    }

    bool resetSelectedSoundForSmoke() override
    {
        return soundStatePanel_->resetSelectionForSmoke();
    }

    bool captureSelectedSoundForSmoke() override
    {
        return soundStatePanel_->captureSelectionForSmoke();
    }

    bool soundStateReadyForSmoke(
        std::string_view variableId,
        std::string_view selectedDefinitionId,
        bool assignmentEnabled) const override
    {
        return soundMode_
            && soundStatePanel_->surfaceReadyForSmoke(
                variableId,
                selectedDefinitionId,
                assignmentEnabled);
    }

private:
    struct DeclarationIdentity
    {
        talas::project::ProjectVariableOwner owner;
        std::string variableId;
        bool operator==(const DeclarationIdentity&) const = default;
    };

    struct PreparedChoice
    {
        talas::project::PreparedProjectVariableIdentity identity;
        std::uint64_t voiceGeneration = 0;

        bool operator==(const PreparedChoice&) const = default;
    };

    void setSoundMode(bool enabled)
    {
        soundMode_ = enabled;
        scalarModeButton_.setToggleState(!enabled, juce::dontSendNotification);
        soundModeButton_.setToggleState(enabled, juce::dontSendNotification);
        for (auto* component : std::initializer_list<juce::Component*>{
                 &declarationBox_, &newButton_, &variableId_, &displayName_,
                 &ownerBox_, &domainBox_, &initialValue_, &snapshotEligible_,
                 &saveButton_, &deleteButton_, &instanceBox_, &liveValue_,
                 &assignValue_, &assignButton_, &resetButton_, &resetAllButton_,
                 &captureButton_, &status_})
        {
            component->setVisible(!enabled);
        }
        soundStatePanel_->setVisible(enabled);
        if (enabled)
            soundStatePanel_->refreshLiveValues();
        resized();
    }

    void rebuildOwners()
    {
        std::optional<talas::project::ProjectVariableOwner> retained;
        const auto retainedIndex = ownerBox_.getSelectedItemIndex();
        if (retainedIndex >= 0
            && static_cast<std::size_t>(retainedIndex)
                < ownerChoices_.size())
        {
            retained = ownerChoices_[static_cast<std::size_t>(retainedIndex)];
        }
        ownerChoices_.clear();
        ownerBox_.clear(juce::dontSendNotification);
        ownerChoices_.push_back({});
        ownerBox_.addItem("Project", 1);
        ownerChoices_.push_back({
            talas::project::ProjectVariableOwnerKind::WorkspaceSession});
        ownerBox_.addItem("Workspace Session | local to this project", 2);
        const auto appendVoiceOwners = [&](
            const auto& graph,
            std::string_view definitionId,
            const juce::String& structuralPrefix) {
            for (const auto& node : graph.nodes)
            {
                if (node.typeId != "talas.midiVoice")
                    continue;
                ownerChoices_.push_back({
                    talas::project::ProjectVariableOwnerKind::MidiVoice,
                    std::string{definitionId},
                    node.instanceId,
                });
                ownerBox_.addItem(
                    "MIDI Voice | " + structuralPrefix + " / "
                        + juceString(node.instanceId),
                    static_cast<int>(ownerChoices_.size()));
            }
        };
        appendVoiceOwners(
            bridge_.editor().document().graph,
            {},
            "Project");
        for (const auto& definition : bridge_.editor().document().soundDefinitions)
        {
            ownerChoices_.push_back({
                talas::project::ProjectVariableOwnerKind::SoundDefinition,
                definition.definitionId});
            ownerBox_.addItem(
                "Sound | " + juceString(definition.displayName)
                    + " [" + juceString(definition.definitionId) + "]",
                static_cast<int>(ownerChoices_.size()));
            appendVoiceOwners(
                definition.graph,
                definition.definitionId,
                "Sound " + juceString(definition.displayName)
                    + " [" + juceString(definition.definitionId) + "]");
        }
        if (retained.has_value())
        {
            const auto found = std::ranges::find(ownerChoices_, *retained);
            if (found != ownerChoices_.end())
            {
                ownerBox_.setSelectedItemIndex(
                    static_cast<int>(
                        std::distance(ownerChoices_.begin(), found)),
                    juce::dontSendNotification);
            }
        }
        if (ownerBox_.getSelectedId() == 0)
            ownerBox_.setSelectedId(1, juce::dontSendNotification);
    }

    void selectDeclaration()
    {
        const auto index = declarationBox_.getSelectedItemIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= declarationChoices_.size())
            return;
        loadDeclaration(declarationChoices_[static_cast<std::size_t>(index)]);
    }

    void loadDeclaration(const DeclarationIdentity& identity)
    {
        const auto& variables = bridge_.editor().document().variableLibrary.variables;
        const auto found = std::ranges::find_if(
            variables,
            [&](const auto& declaration) {
                return declaration.owner == identity.owner
                    && declaration.variableId == identity.variableId;
            });
        if (found == variables.end())
            return;
        selectedDeclaration_ = identity;
        variableId_.setText(juceString(found->variableId), false);
        displayName_.setText(juceString(found->displayName), false);
        domainBox_.setSelectedId(domainId(found->domain), juce::dontSendNotification);
        initialValue_.setText(juce::String(found->initialValue, 12), false);
        snapshotEligible_.setToggleState(found->snapshotEligible, juce::dontSendNotification);
        snapshotEligible_.setEnabled(found->owner.kind
            != talas::project::ProjectVariableOwnerKind::MidiVoice);
        const auto owner = std::ranges::find(ownerChoices_, found->owner);
        ownerBox_.setSelectedItemIndex(
            owner == ownerChoices_.end()
                ? 0
                : static_cast<int>(std::distance(ownerChoices_.begin(), owner)),
            juce::dontSendNotification);
        auto identityText = juceString(
            std::to_string(static_cast<int>(found->owner.kind)));
        identityText += ":" + juceString(found->owner.definitionId)
            + ":" + juceString(found->owner.nodeId)
            + ":" + juceString(found->variableId);
        const auto prefix = "inspector.variable:" + identityText + ":";
        declarationBox_.setComponentID("inspector.variables.list:" + identityText);
        variableId_.setComponentID(prefix + "id");
        displayName_.setComponentID(prefix + "display-name");
        ownerBox_.setComponentID(prefix + "owner");
        domainBox_.setComponentID(prefix + "domain");
        initialValue_.setComponentID(prefix + "initial");
        snapshotEligible_.setComponentID(prefix + "snapshot");
        saveButton_.setComponentID(prefix + "save");
        deleteButton_.setComponentID(prefix + "delete");
        deleteButton_.setEnabled(true);
        saveButton_.setEnabled(true);
        pendingDestructiveIdentity_.reset();
        pendingDeclarationEdit_.reset();
        refreshLiveValues();
    }

    void beginNew()
    {
        selectedDeclaration_.reset();
        selectedPreparedChoice_.reset();
        variableId_.clear();
        displayName_.clear();
        ownerBox_.setSelectedId(1, juce::dontSendNotification);
        domainBox_.setSelectedId(1, juce::dontSendNotification);
        initialValue_.setText("0", false);
        snapshotEligible_.setToggleState(true, juce::dontSendNotification);
        snapshotEligible_.setEnabled(true);
        deleteButton_.setEnabled(false);
        saveButton_.setEnabled(
            talas::expr::supportsVariableReferences(
                bridge_.editor().document().expressionLanguage));
        declarationBox_.setComponentID("inspector.variables.list");
        variableId_.setComponentID("inspector.variable:new:id");
        instanceBox_.clear(juce::dontSendNotification);
        liveValue_.setText("Unavailable | declaration not saved", false);
        status_.setText("New variable | enter exact owner, @variableId, domain and initial value", false);
        pendingDestructiveIdentity_.reset();
        pendingDeclarationEdit_.reset();
    }

    [[nodiscard]] std::optional<talas::project::ProjectVariableDeclaration>
    editedDeclaration() const
    {
        const auto domain = selectedDomain(domainBox_);
        const auto ownerIndex = ownerBox_.getSelectedItemIndex();
        const auto value = parseFiniteNumber(initialValue_.getText());
        if (!domain.has_value() || ownerIndex < 0
            || static_cast<std::size_t>(ownerIndex) >= ownerChoices_.size()
            || !value.has_value())
            return std::nullopt;
        talas::project::ProjectVariableDeclaration declaration;
        declaration.variableId = variableId_.getText().trim().toStdString();
        declaration.displayName = displayName_.getText().trim().toStdString();
        declaration.owner = ownerChoices_[static_cast<std::size_t>(ownerIndex)];
        declaration.domain = *domain;
        declaration.initialValue = *value;
        declaration.snapshotEligible = declaration.owner.kind
                == talas::project::ProjectVariableOwnerKind::MidiVoice
            ? false
            : snapshotEligible_.getToggleState();
        return declaration;
    }

    void saveDeclaration()
    {
        const auto declaration = editedDeclaration();
        if (!declaration.has_value())
        {
            onStatus_("Variable edit blocked: owner, domain, and finite initial value are required");
            return;
        }
        const auto nextIdentity = DeclarationIdentity{
            declaration->owner, declaration->variableId};
        talas::project::ProjectVariableReferencePaths references;
        const auto& variables =
            bridge_.editor().document().variableLibrary.variables;
        const auto current = selectedDeclaration_.has_value()
            ? std::ranges::find_if(
                  variables,
                  [&](const auto& candidate) {
                      return candidate.owner == selectedDeclaration_->owner
                          && candidate.variableId
                              == selectedDeclaration_->variableId;
                  })
            : variables.end();
        if (current != variables.end() && *current != *declaration)
        {
            try
            {
                references = talas::project::findProjectVariableReferencePaths(
                    bridge_.editor().document(),
                    current->owner,
                    current->variableId);
            }
            catch (const std::bad_alloc&)
            {
                onStatus_("Variable edit blocked: reference-path allocation failed");
                return;
            }
            catch (const std::length_error&)
            {
                onStatus_("Variable edit blocked: reference paths exceeded supported limits");
                return;
            }
            if (references.truncated)
            {
                onStatus_("Variable edit blocked: complete reference-path confirmation exceeds supported limits");
                return;
            }
            if ((!references.paths.empty() || references.truncated)
                && pendingDeclarationEdit_ != declaration)
            {
                pendingDeclarationEdit_ = *declaration;
                saveButton_.setButtonText("Confirm");
                status_.setText(
                    "Confirm referenced declaration change | exact references "
                        + referencePathsText(references)
                        + " | exact project audit will run again",
                    false);
                return;
            }
        }
        const auto result = selectedDeclaration_.has_value()
            ? bridge_.editor().replaceProjectVariable(
                  selectedDeclaration_->owner,
                  selectedDeclaration_->variableId,
                  *declaration)
            : bridge_.editor().upsertProjectVariable(*declaration);
        saveButton_.setButtonText("Save");
        pendingDeclarationEdit_.reset();
        if (!result.ok())
        {
            status_.setText("Edit blocked | " + juceString(result.message), false);
            onStatus_("Variable edit blocked: " + juceString(result.message));
            return;
        }
        selectedDeclaration_ = nextIdentity;
        onDocumentChange_();
        onStatus_("Variable declaration saved as one Undo step | fresh staging required");
        refresh();
    }

    void deleteDeclaration()
    {
        if (!selectedDeclaration_.has_value())
            return;
        if (pendingDestructiveIdentity_ != selectedDeclaration_)
        {
            auto referenceText = juce::String{"unreferenced"};
            try
            {
                const auto references =
                    talas::project::findProjectVariableReferencePaths(
                        bridge_.editor().document(),
                        selectedDeclaration_->owner,
                        selectedDeclaration_->variableId);
                if (references.truncated)
                {
                    onStatus_("Variable delete blocked: complete reference-path confirmation exceeds supported limits");
                    return;
                }
                if (!references.paths.empty())
                    referenceText = "exact references "
                        + referencePathsText(references);
            }
            catch (const std::bad_alloc&)
            {
                onStatus_("Variable delete blocked: reference-path allocation failed");
                return;
            }
            catch (const std::length_error&)
            {
                onStatus_("Variable delete blocked: reference paths exceeded supported limits");
                return;
            }
            pendingDestructiveIdentity_ = selectedDeclaration_;
            deleteButton_.setButtonText("Confirm");
            status_.setText(
                "Confirm deletion of @" + juceString(selectedDeclaration_->variableId)
                    + " | " + referenceText,
                false);
            return;
        }
        const auto deleted = *selectedDeclaration_;
        const auto deletedIndex = std::ranges::find(
            declarationChoices_, deleted) - declarationChoices_.begin();
        const auto result = bridge_.editor().removeProjectVariable(
            deleted.owner, deleted.variableId);
        deleteButton_.setButtonText("Delete");
        if (!result.ok())
        {
            pendingDestructiveIdentity_.reset();
            status_.setText("Delete blocked | " + juceString(result.message), false);
            onStatus_("Variable delete blocked: " + juceString(result.message));
            return;
        }
        selectedDeclaration_.reset();
        const auto& remaining =
            bridge_.editor().document().variableLibrary.variables;
        if (!remaining.empty())
        {
            const auto neighbor = std::min<std::size_t>(
                static_cast<std::size_t>(deletedIndex),
                remaining.size() - 1U);
            selectedDeclaration_ = DeclarationIdentity{
                remaining[neighbor].owner,
                remaining[neighbor].variableId};
        }
        onDocumentChange_();
        onStatus_("Variable declaration deleted as one Undo step | fresh staging required");
        refresh();
    }

    void rebuildInstances()
    {
        const auto retained = selectedPreparedChoice_;
        preparedChoices_.clear();
        instanceBox_.clear(juce::dontSendNotification);
        if (!selectedDeclaration_.has_value())
            return;
        latestValues_ = bridge_.preparedVariableValues();
        if (!latestValues_.publicationValid)
            return;
        const auto& variables = bridge_.editor().document().variableLibrary.variables;
        const auto declaration = std::ranges::find_if(
            variables,
            [&](const auto& candidate) {
                return candidate.owner == selectedDeclaration_->owner
                    && candidate.variableId == selectedDeclaration_->variableId;
            });
        if (declaration == variables.end())
            return;
        for (const auto& value : latestValues_.values)
        {
            if (!declarationMatches(*declaration, value.identity))
                continue;
            preparedChoices_.push_back({
                value.identity,
                value.voiceGeneration,
            });
            auto label = instancePathText(value.identity);
            if (value.identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice)
            {
                label += value.voiceGeneration == 0
                    ? " | inactive"
                    : " | generation "
                        + juce::String(static_cast<juce::int64>(
                            value.voiceGeneration))
                        + " | ch "
                        + juce::String(static_cast<int>(value.midiChannel))
                        + " note "
                        + juce::String(static_cast<int>(value.midiNote))
                        + " velocity "
                        + juce::String(static_cast<int>(value.midiVelocity))
                        + " | " + (value.voiceActive
                                ? juce::String{"active"}
                                : value.voiceReleasing
                                    ? juce::String{"release"}
                                    : juce::String{"inactive"});
            }
            instanceBox_.addItem(
                label,
                static_cast<int>(preparedChoices_.size()));
        }
        if (retained.has_value())
        {
            const auto found = std::ranges::find(preparedChoices_, *retained);
            if (found != preparedChoices_.end())
            {
                instanceBox_.setSelectedItemIndex(
                    static_cast<int>(std::distance(preparedChoices_.begin(), found)),
                    juce::dontSendNotification);
                selectedPreparedChoice_ = *found;
                updatePreparedFocusIdentity();
                return;
            }
            if (retained->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice)
            {
                selectedPreparedChoice_.reset();
                instanceBox_.setSelectedItemIndex(-1, juce::dontSendNotification);
                updatePreparedFocusIdentity();
                return;
            }
        }
        if (!preparedChoices_.empty())
        {
            instanceBox_.setSelectedItemIndex(0, juce::dontSendNotification);
            selectedPreparedChoice_ = preparedChoices_.front();
        }
        else
        {
            selectedPreparedChoice_.reset();
        }
        updatePreparedFocusIdentity();
    }

    void selectInstance()
    {
        const auto index = instanceBox_.getSelectedItemIndex();
        if (index < 0 || static_cast<std::size_t>(index) >= preparedChoices_.size())
            selectedPreparedChoice_.reset();
        else
            selectedPreparedChoice_ = preparedChoices_[static_cast<std::size_t>(index)];
        updatePreparedFocusIdentity();
        updateLivePresentation();
    }

    void updatePreparedFocusIdentity()
    {
        if (!selectedPreparedChoice_.has_value())
        {
            instanceBox_.setComponentID("inspector.variable:instance:unavailable");
            assignButton_.setComponentID("inspector.variable:assign:unavailable");
            resetButton_.setComponentID("inspector.variable:reset:unavailable");
            describeControl(
                instanceBox_,
                "Prepared variable instance unavailable",
                "No exact active-stage cell or MIDI Voice slot generation is selected.");
            return;
        }
        const auto& choice = *selectedPreparedChoice_;
        auto exact = juceString(
            std::to_string(static_cast<int>(choice.identity.ownerKind)));
        exact += ":" + juceString(choice.identity.definitionId)
            + ":" + juceString(choice.identity.ownerNodeId);
        for (const auto& segment : choice.identity.instancePath)
            exact += ":" + juceString(segment);
        exact += ":" + juceString(choice.identity.variableId);
        if (choice.identity.voiceOrdinal.has_value())
            exact += ":slot"
                + juce::String(
                    static_cast<int>(*choice.identity.voiceOrdinal));
        exact += ":generation"
            + juce::String(static_cast<juce::int64>(choice.voiceGeneration));
        instanceBox_.setComponentID("inspector.variable:instance:" + exact);
        assignButton_.setComponentID("inspector.variable:assign:" + exact);
        resetButton_.setComponentID("inspector.variable:reset:" + exact);
        const auto voice = choice.identity.ownerKind
            == talas::project::ProjectVariableOwnerKind::MidiVoice;
        const auto exactDescription = instancePathText(choice.identity)
            + (voice
                    ? " | exact slot generation "
                        + juce::String(static_cast<juce::int64>(
                            choice.voiceGeneration))
                    : juce::String{});
        describeControl(
            instanceBox_,
            "Prepared variable instance: " + exactDescription,
            voice
                ? "Runtime-only MIDI Voice cell. Assignment and Reset require this exact active slot generation and never redirect after stealing."
                : "Prepared shared variable cell bound to the exact active stage and structural owner identity.");
        describeControl(
            assignButton_,
            "Set live @" + juceString(choice.identity.variableId)
                + " for " + exactDescription,
            voice
                ? "Queues one exact stage-and-slot-generation assignment."
                : "Queues one exact active-stage variable assignment.");
        describeControl(
            resetButton_,
            "Reset @" + juceString(choice.identity.variableId)
                + " for " + exactDescription,
            "Restores only this exact prepared cell to its declared initial value.");
    }

    void updateLivePresentation()
    {
        const auto enabled = selectedPreparedChoice_.has_value()
            && latestValues_.publicationValid
            && latestValues_.stageId == bridge_.activeStageId();
        const auto value = enabled
            ? std::ranges::find(
                  latestValues_.values,
                  selectedPreparedChoice_->identity,
                  &talas::runtime::PreparedVariableValue::identity)
            : latestValues_.values.end();
        const auto exactVoiceGeneration = value != latestValues_.values.end()
            && (value->identity.ownerKind
                    != talas::project::ProjectVariableOwnerKind::MidiVoice
                || (value->voiceGeneration != 0
                    && value->voiceGeneration
                        == selectedPreparedChoice_->voiceGeneration));
        const auto found = enabled && value != latestValues_.values.end()
            && exactVoiceGeneration;
        assignButton_.setEnabled(found);
        resetButton_.setEnabled(found);
        captureButton_.setEnabled(found
            && selectedPreparedChoice_->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::Project);
        resetAllButton_.setEnabled(latestValues_.publicationValid
            && latestValues_.stageId == bridge_.activeStageId()
            && std::ranges::any_of(
                latestValues_.values,
                [](const talas::runtime::PreparedVariableValue& candidate) {
                    return candidate.identity.ownerKind
                        != talas::project::ProjectVariableOwnerKind::MidiVoice;
                }));
        if (!found)
        {
            liveValue_.setText("Unavailable | stage or instance is stale", false);
            status_.setText(
                selectedDeclaration_.has_value()
                    ? "Declared initial retained | no coherent active-stage value"
                    : "No variable selected",
                false);
            return;
        }
        const auto& variables = bridge_.editor().document().variableLibrary.variables;
        const auto declaration = std::ranges::find_if(
            variables,
            [&](const auto& candidate) {
                return selectedDeclaration_.has_value()
                    && candidate.owner == selectedDeclaration_->owner
                    && candidate.variableId == selectedDeclaration_->variableId;
            });
        const auto current = value->value.value;
        liveValue_.setText(
            "Current " + juce::String(current, 12)
                + " " + domainText(value->value.domain),
            false);
        if (declaration != variables.end())
        {
            const auto modified = current != declaration->initialValue;
            status_.setText(
                "@" + juceString(declaration->variableId)
                    + " | " + instancePathText(value->identity)
                    + " | initial " + juce::String(declaration->initialValue, 12)
                    + " | " + (modified ? "modified" : "at initial")
                    + " | snapshots "
                    + (declaration->snapshotEligible ? "eligible" : "excluded")
                    + (declaration->owner.kind
                            == talas::project::ProjectVariableOwnerKind::WorkspaceSession
                        ? " | local persistence after callback acknowledgment"
                        : declaration->owner.kind
                                == talas::project::ProjectVariableOwnerKind::MidiVoice
                            ? " | runtime-only exact voice generation"
                            : " | project-owned declaration"),
                false);
        }
    }

    bool assignLive()
    {
        if (!selectedPreparedChoice_.has_value())
            return false;
        const auto value = parseFiniteNumber(assignValue_.getText());
        const auto domain = selectedDomain(domainBox_);
        if (!value.has_value() || !domain.has_value())
            return false;
        const auto accepted = selectedPreparedChoice_->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice
            ? bridge_.scheduleVoiceVariableAssignment(
                selectedPreparedChoice_->identity,
                selectedPreparedChoice_->voiceGeneration,
                *value,
                *domain,
                0,
                bridge_.latestStageGeneration())
            : bridge_.scheduleVariableAssignment(
                selectedPreparedChoice_->identity,
                *value,
                *domain,
                0,
                bridge_.latestStageGeneration());
        onStatus_(accepted
            ? "Variable assignment queued for the exact active stage"
            : "Variable assignment rejected: stage or instance authority is stale");
        if (accepted)
            onLiveMutation_();
        return accepted;
    }

    bool resetSelected()
    {
        if (!selectedPreparedChoice_.has_value())
            return false;
        bool accepted = false;
        if (selectedPreparedChoice_->identity.ownerKind
            == talas::project::ProjectVariableOwnerKind::MidiVoice)
        {
            const auto declaration = selectedDeclaration_.has_value()
                ? std::ranges::find_if(
                    bridge_.editor().document().variableLibrary.variables,
                    [&](const auto& candidate) {
                        return candidate.owner == selectedDeclaration_->owner
                            && candidate.variableId
                                == selectedDeclaration_->variableId;
                    })
                : bridge_.editor().document().variableLibrary.variables.end();
            accepted = declaration
                    != bridge_.editor().document().variableLibrary.variables.end()
                && bridge_.scheduleVoiceVariableAssignment(
                    selectedPreparedChoice_->identity,
                    selectedPreparedChoice_->voiceGeneration,
                    declaration->initialValue,
                    declaration->domain,
                    0,
                    bridge_.latestStageGeneration());
        }
        else
        {
            const std::array identities{selectedPreparedChoice_->identity};
            accepted = bridge_.resetVariables(
                    identities, bridge_.latestStageGeneration())
                == talas::runtime::SnapshotDirectTransactionResult::Accepted;
        }
        onStatus_(accepted
            ? "Selected variable reset to its declared initial value"
            : "Variable reset rejected: stage or transaction authority is stale");
        if (accepted)
            onLiveMutation_();
        return accepted;
    }

    void resetAll()
    {
        const auto result = bridge_.resetAllVariables(
            bridge_.latestStageGeneration());
        if (result == talas::runtime::SnapshotDirectTransactionResult::Accepted)
            onLiveMutation_();
        onStatus_(result == talas::runtime::SnapshotDirectTransactionResult::Accepted
            ? "All shared prepared variables reset in one transaction | voice slots retain exact generation state"
            : "Reset shared rejected: stage or transaction authority is stale");
    }

    bool captureInitial()
    {
        if (!selectedPreparedChoice_.has_value())
            return false;
        const auto result = bridge_.captureProjectVariableInitialValue(
            selectedPreparedChoice_->identity,
            bridge_.latestStageGeneration(),
            bridge_.editor().documentAuthorityGeneration());
        if (!result.ok())
        {
            onStatus_("Variable capture blocked: " + juceString(result.message));
            return false;
        }
        onDocumentChange_();
        onStatus_("Captured current project variable as its initial value | fresh staging required");
        refresh();
        return true;
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    LiveMutationCallback onLiveMutation_;
    StatusCallback onStatus_;
    juce::TextButton scalarModeButton_;
    juce::TextButton soundModeButton_;
    std::unique_ptr<SoundStatePanel> soundStatePanel_;
    bool soundMode_ = false;
    std::vector<DeclarationIdentity> declarationChoices_;
    std::vector<talas::project::ProjectVariableOwner> ownerChoices_;
    std::vector<PreparedChoice> preparedChoices_;
    std::optional<DeclarationIdentity> selectedDeclaration_;
    std::optional<DeclarationIdentity> pendingDestructiveIdentity_;
    std::optional<talas::project::ProjectVariableDeclaration>
        pendingDeclarationEdit_;
    std::optional<PreparedChoice> selectedPreparedChoice_;
    talas::runtime::PreparedVariableValueSnapshot latestValues_;
    juce::Label title_;
    juce::ComboBox declarationBox_;
    juce::TextButton newButton_;
    juce::TextEditor variableId_;
    juce::TextEditor displayName_;
    juce::ComboBox ownerBox_;
    juce::ComboBox domainBox_;
    juce::TextEditor initialValue_;
    juce::ToggleButton snapshotEligible_;
    juce::TextButton saveButton_;
    juce::TextButton deleteButton_;
    juce::ComboBox instanceBox_;
    juce::TextEditor liveValue_;
    juce::TextEditor assignValue_;
    juce::TextButton assignButton_;
    juce::TextButton resetButton_;
    juce::TextButton resetAllButton_;
    juce::TextButton captureButton_;
    juce::TextEditor status_;
};
}

std::unique_ptr<ProjectVariablesPanel> makeProjectVariablesPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    ProjectVariablesPanel::DocumentChangeCallback onDocumentChange,
    ProjectVariablesPanel::LiveMutationCallback onLiveMutation,
    ProjectVariablesPanel::StatusCallback onStatus)
{
    return std::make_unique<ProjectVariablesPanelImpl>(
        bridge,
        std::move(onDocumentChange),
        std::move(onLiveMutation),
        std::move(onStatus));
}
}
