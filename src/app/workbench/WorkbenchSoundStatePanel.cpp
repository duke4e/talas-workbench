#include "app/workbench/WorkbenchSoundStatePanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/project/ProjectSoundVariables.h"

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
struct DeclarationIdentity
{
    talas::project::ProjectVariableOwner owner;
    std::string variableId;

    bool operator==(const DeclarationIdentity&) const = default;
};

struct PreparedChoice
{
    talas::project::PreparedProjectSoundVariableIdentity identity;
    std::uint64_t voiceGeneration = 0;

    bool operator==(const PreparedChoice&) const = default;
};

[[nodiscard]] juce::String ownerText(
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

[[nodiscard]] juce::String preparedOwnerText(
    const talas::project::PreparedProjectSoundVariableIdentity& identity)
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
    auto result = identity.definitionId.empty()
        ? juce::String{"Project"}
        : "Sound " + juceString(identity.definitionId);
    for (const auto& segment : identity.instancePath)
        result += " / " + juceString(segment);
    if (identity.ownerKind
        == talas::project::ProjectVariableOwnerKind::MidiVoice)
    {
        result += " / MIDI Voice " + juceString(identity.ownerNodeId);
        if (identity.voiceOrdinal.has_value())
            result += " / slot "
                + juce::String(
                    static_cast<int>(*identity.voiceOrdinal) + 1);
    }
    return result;
}

[[nodiscard]] juce::String definitionName(
    const talas::project::ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find(
        document.soundDefinitions,
        definitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end()
        ? "Missing [" + juceString(definitionId) + "]"
        : juceString(found->displayName) + " ["
            + juceString(found->definitionId) + "]";
}

[[nodiscard]] juce::String framed(std::string_view value)
{
    return juceString(std::to_string(value.size())) + ":"
        + juceString(value);
}

[[nodiscard]] juce::String ownerIdentity(
    const talas::project::ProjectVariableOwner& owner)
{
    return juceString(std::to_string(static_cast<int>(owner.kind)))
        + ":" + framed(owner.definitionId) + ":"
        + framed(owner.nodeId);
}

[[nodiscard]] const talas::project::ProjectParameterExpression*
variableIdParameter(const talas::project::ProjectNode& node)
{
    const auto found = std::ranges::find(
        node.parameters,
        std::string_view{"variableId"},
        &talas::project::ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] juce::String referencePathsText(
    const talas::project::ProjectDocument& document,
    const DeclarationIdentity& identity)
{
    juce::String result;
    std::size_t count = 0;
    auto append = [&](const juce::String& path) {
        if (count >= talas::project::maximumProjectSoundVariableDiagnostics)
            return;
        if (count++ != 0)
            result += "; ";
        result += path;
    };
    const auto scanGraph = [&](const auto& graph, const juce::String& prefix) {
        for (const auto& node : graph.nodes)
        {
            if (node.typeId != "talas.variable"
                && node.typeId != "talas.soundCollectionVariable")
            {
                continue;
            }
            const auto* parameter = variableIdParameter(node);
            if (parameter != nullptr && parameter->text.has_value()
                && *parameter->text == identity.variableId)
            {
                append(prefix + "/nodes/" + juceString(node.instanceId)
                    + "/parameters/variableId");
            }
        }
    };
    scanGraph(document.graph, "$.graph");
    for (const auto& definition : document.soundDefinitions)
    {
        scanGraph(
            definition.graph,
            "$.soundDefinitions[" + juceString(definition.definitionId)
                + "].graph");
    }
    for (const auto& bank : document.snapshotLibrary.banks)
    {
        for (const auto& snapshot : bank.snapshots)
        {
            for (const auto& value : snapshot.values)
            {
                if (value.target.kind
                        == talas::project::SnapshotTargetKind::SoundVariable
                    && value.target.variableId == identity.variableId)
                {
                    append(
                        "$.snapshotLibrary/" + juceString(bank.bankId)
                        + "/" + juceString(snapshot.snapshotId)
                        + "/" + juceString(
                            talas::project::snapshotTargetIdentityString(
                                value.target)));
                }
            }
        }
    }
    if (result.isEmpty())
        return "no graph or snapshot references";
    if (count >= talas::project::maximumProjectSoundVariableDiagnostics)
        result += "; additional paths omitted at the bounded diagnostic limit";
    return result;
}

[[nodiscard]] bool declarationMatches(
    const talas::project::ProjectSoundVariableDeclaration& declaration,
    const talas::project::PreparedProjectSoundVariableIdentity& identity) noexcept
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

class SoundStatePanelImpl final : public SoundStatePanel
{
public:
    SoundStatePanelImpl(
        talas::runtime::ProjectRuntimeBridge& bridge,
        DocumentChangeCallback onDocumentChange,
        LiveMutationCallback onLiveMutation,
        StatusCallback onStatus)
        : bridge_(bridge)
        , onDocumentChange_(std::move(onDocumentChange))
        , onLiveMutation_(std::move(onLiveMutation))
        , onStatus_(std::move(onStatus))
        , newButton_("New")
        , addCandidateButton_("Add")
        , removeCandidateButton_("Remove")
        , moveCandidateUpButton_("Up")
        , moveCandidateDownButton_("Down")
        , saveButton_("Save")
        , deleteButton_("Delete")
        , assignButton_("Select live")
        , resetButton_("Reset")
        , resetAllButton_("Reset shared")
        , captureButton_("Capture initial")
    {
        setComponentID("inspector.sound-state.surface");
        describeControl(
            *this,
            "Sound State",
            "Bounded Sound and SoundCollection declarations with exact reusable-Sound identity, prepared instance state, snapshot policy, and stage-bound live selection.");
        for (auto* component : std::initializer_list<juce::Component*>{
                 &declarationBox_, &newButton_, &variableId_, &ownerBox_,
                 &displayName_, &kindBox_, &snapshotEligible_,
                 &candidateBox_, &candidateDefinitionBox_,
                 &addCandidateButton_, &removeCandidateButton_,
                 &moveCandidateUpButton_, &moveCandidateDownButton_,
                 &initialSelectionBox_, &saveButton_, &deleteButton_,
                 &instanceBox_, &liveValue_, &liveSelectionBox_,
                 &assignButton_, &resetButton_, &resetAllButton_,
                 &captureButton_, &status_})
        {
            addAndMakeVisible(*component);
        }

        declarationBox_.setComponentID("inspector.sound-state.list");
        newButton_.setComponentID("inspector.sound-state.new");
        variableId_.setComponentID("inspector.sound-state:new:id");
        ownerBox_.setComponentID("inspector.sound-state:new:owner");
        displayName_.setComponentID("inspector.sound-state:new:display-name");
        kindBox_.setComponentID("inspector.sound-state:new:kind");
        snapshotEligible_.setComponentID("inspector.sound-state:new:snapshot");
        candidateBox_.setComponentID("inspector.sound-state:new:candidate");
        candidateDefinitionBox_.setComponentID("inspector.sound-state:new:add-candidate");
        initialSelectionBox_.setComponentID("inspector.sound-state:new:initial");
        instanceBox_.setComponentID("inspector.sound-state:instance:unavailable");
        liveValue_.setComponentID("inspector.sound-state:live:unavailable");
        liveSelectionBox_.setComponentID("inspector.sound-state:selection:unavailable");
        status_.setComponentID("inspector.sound-state:status");

        variableId_.setTextToShowWhenEmpty("variableId", secondaryTextColour);
        variableId_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectVariableIdentifierBytes));
        displayName_.setTextToShowWhenEmpty("Display name", secondaryTextColour);
        displayName_.setInputRestrictions(static_cast<int>(
            talas::project::maximumProjectVariableDisplayNameBytes));
        snapshotEligible_.setButtonText("Snapshots");
        kindBox_.addItem("Sound", 1);
        kindBox_.addItem("SoundCollection", 2);
        kindBox_.setSelectedId(1, juce::dontSendNotification);
        liveValue_.setReadOnly(true);
        liveValue_.setCaretVisible(false);
        status_.setMultiLine(true);
        status_.setReadOnly(true);
        status_.setCaretVisible(false);
        status_.setFont(WorkbenchType::micro());
        for (auto* editor : {&liveValue_, &status_})
        {
            editor->setColour(
                juce::TextEditor::backgroundColourId,
                workbenchTheme.controlTile);
            editor->setColour(
                juce::TextEditor::outlineColourId,
                panelBorderColour);
            editor->setColour(
                juce::TextEditor::textColourId,
                secondaryTextColour);
        }

        describeControl(declarationBox_, "Sound State declaration", "Select one stable variable and exact owner; focus identity is variable-based, never row-based.");
        describeControl(newButton_, "New Sound State declaration", "Prepare one bounded Sound or SoundCollection declaration.");
        describeControl(variableId_, "Sound State identifier", "Stable bounded identity consumed by talas.variable or talas.soundCollectionVariable through its exact variableId field.");
        describeControl(ownerBox_, "Sound State owner", "Project, Workspace Session, reusable Sound definition, or exact MIDI Voice structural owner.");
        describeControl(displayName_, "Sound State display name", "Human-readable label; it never replaces the stable variable identity.");
        describeControl(kindBox_, "Sound State kind", "Sound or ordered SoundCollection. Kind changes require confirmation and fresh staging.");
        describeControl(snapshotEligible_, "Sound State snapshot eligibility", "Includes the exact selected reusable-Sound identity in discrete snapshot recall and morph.");
        describeControl(candidateBox_, "Ordered Sound candidates", "Select one exact reusable-Sound definition already present in the declaration.");
        describeControl(candidateDefinitionBox_, "Available reusable Sound", "Choose one exact source Sound definition to append to the bounded candidate list.");
        describeControl(addCandidateButton_, "Add Sound candidate", "Adds the selected exact definition to the local edit; Save commits membership as one Undo step.");
        describeControl(removeCandidateButton_, "Remove Sound candidate", "Removes the selected candidate from the local edit; referenced changes require confirmation before Save.");
        describeControl(moveCandidateUpButton_, "Move Sound candidate earlier", "Reorders presentation while retaining initial/live authority by exact definition ID.");
        describeControl(moveCandidateDownButton_, "Move Sound candidate later", "Reorders presentation while retaining initial/live authority by exact definition ID.");
        describeControl(initialSelectionBox_, "Initial Sound selection", "Exact candidate restored on Reset and fresh preparation; empty collections remain explicit silence.");
        describeControl(saveButton_, "Save Sound State declaration", "Validates all candidates and commits one failure-atomic Undo step after exact reference confirmation when required.");
        describeControl(deleteButton_, "Delete Sound State declaration", "Requires explicit confirmation with graph and snapshot reference paths.");
        describeControl(instanceBox_, "Prepared Sound State instance", "Exact project/session cell, reusable-Sound instance path, or MIDI Voice slot ordinal and generation.");
        describeControl(liveValue_, "Current Sound selection", "Coherent selected definition from the active stage; stale or missing publication is shown as unavailable.");
        describeControl(liveSelectionBox_, "New live Sound selection", "One member of the immutable prepared candidate list; membership edits require restaging.");
        describeControl(assignButton_, "Select live Sound", "Queues one stage-bound selection; voice state also requires the exact slot generation.");
        describeControl(resetButton_, "Reset selected Sound State", "Restores the exact selected prepared cell to its declared initial candidate.");
        describeControl(resetAllButton_, "Reset shared Sound State", "Resets all non-voice prepared Sound cells in one bounded transaction.");
        describeControl(captureButton_, "Capture initial Sound", "Copies one coherent project-owned live selection into the declaration as one Undo step.");
        describeControl(status_, "Sound State status", "Exact owner, kind, candidate count, initial/current selection, snapshot policy, stage availability, and functional-subset limits.");

        declarationBox_.onChange = [this] { selectDeclaration(); };
        candidateBox_.onChange = [this] { updateCandidateButtons(); };
        candidateDefinitionBox_.onChange = [this] {
            updateDefinitionChoiceFocusIdentity();
        };
        initialSelectionBox_.onChange = [this] {
            const auto index = initialSelectionBox_.getSelectedItemIndex();
            if (index >= 0
                && static_cast<std::size_t>(index) < candidateIds_.size())
            {
                initialDefinitionId_ =
                    candidateIds_[static_cast<std::size_t>(index)];
            }
            updateInitialFocusIdentity();
        };
        liveSelectionBox_.onChange = [this] {
            updateLiveSelectionFocusIdentity();
        };
        ownerBox_.onChange = [this] {
            const auto voice = selectedOwner().has_value()
                && selectedOwner()->kind
                    == talas::project::ProjectVariableOwnerKind::MidiVoice;
            if (voice)
                snapshotEligible_.setToggleState(false, juce::dontSendNotification);
            snapshotEligible_.setEnabled(!voice);
        };
        newButton_.onClick = [this] { beginNew(); };
        addCandidateButton_.onClick = [this] { addCandidate(); };
        removeCandidateButton_.onClick = [this] { removeCandidate(); };
        moveCandidateUpButton_.onClick = [this] { moveCandidate(-1); };
        moveCandidateDownButton_.onClick = [this] { moveCandidate(1); };
        saveButton_.onClick = [this] { saveDeclaration(); };
        deleteButton_.onClick = [this] { deleteDeclaration(); };
        instanceBox_.onChange = [this] { selectInstance(); };
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
            *this,
            juce::AccessibilityRole::group);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto content = getLocalBounds().reduced(4);
        auto row = content.removeFromTop(24);
        newButton_.setBounds(row.removeFromRight(48));
        row.removeFromRight(4);
        declarationBox_.setBounds(row);
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        ownerBox_.setBounds(row.removeFromRight(
            std::min(130, row.getWidth() / 2)));
        row.removeFromRight(4);
        variableId_.setBounds(row);
        content.removeFromTop(3);
        displayName_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        snapshotEligible_.setBounds(row.removeFromRight(88));
        row.removeFromRight(4);
        kindBox_.setBounds(row);
        content.removeFromTop(3);
        candidateBox_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        const auto actionWidth = std::max(38, std::min(50, row.getWidth() / 7));
        moveCandidateDownButton_.setBounds(row.removeFromRight(actionWidth));
        row.removeFromRight(3);
        moveCandidateUpButton_.setBounds(row.removeFromRight(actionWidth));
        row.removeFromRight(3);
        removeCandidateButton_.setBounds(row.removeFromRight(58));
        row.removeFromRight(3);
        addCandidateButton_.setBounds(row.removeFromRight(42));
        row.removeFromRight(3);
        candidateDefinitionBox_.setBounds(row);
        content.removeFromTop(3);
        initialSelectionBox_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        deleteButton_.setBounds(row.removeFromRight(58));
        row.removeFromRight(4);
        saveButton_.setBounds(row.removeFromRight(58));
        content.removeFromTop(6);
        instanceBox_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        liveValue_.setBounds(content.removeFromTop(24));
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        assignButton_.setBounds(row.removeFromRight(78));
        row.removeFromRight(4);
        liveSelectionBox_.setBounds(row);
        content.removeFromTop(3);
        row = content.removeFromTop(24);
        const auto third = std::max(1, (row.getWidth() - 8) / 3);
        resetButton_.setBounds(row.removeFromLeft(third));
        row.removeFromLeft(4);
        resetAllButton_.setBounds(row.removeFromLeft(third));
        row.removeFromLeft(4);
        captureButton_.setBounds(row);
        content.removeFromTop(5);
        status_.setBounds(content);
    }

    void refresh() override
    {
        const auto retained = selectedDeclaration_;
        pendingEdit_.reset();
        pendingDelete_.reset();
        saveButton_.setButtonText("Save");
        deleteButton_.setButtonText("Delete");
        rebuildOwners();
        rebuildDefinitions();
        declarationChoices_.clear();
        declarationBox_.clear(juce::dontSendNotification);
        const auto& document = bridge_.editor().document();
        for (const auto& declaration : document.soundVariableLibrary.variables)
        {
            declarationChoices_.push_back({
                declaration.owner,
                declaration.variableId});
            declarationBox_.addItem(
                ownerText(declaration.owner) + " | "
                    + juceString(declaration.variableId),
                static_cast<int>(declarationChoices_.size()));
        }
        newButton_.setEnabled(
            declarationChoices_.size()
            < talas::project::maximumProjectSoundVariableCount);
        if (retained.has_value())
        {
            const auto found = std::ranges::find(
                declarationChoices_,
                *retained);
            if (found != declarationChoices_.end())
            {
                declarationBox_.setSelectedItemIndex(
                    static_cast<int>(std::distance(
                        declarationChoices_.begin(),
                        found)),
                    juce::dontSendNotification);
                loadDeclaration(*retained);
                return;
            }
        }
        if (!declarationChoices_.empty())
        {
            declarationBox_.setSelectedItemIndex(
                0,
                juce::dontSendNotification);
            loadDeclaration(declarationChoices_.front());
        }
        else
        {
            beginNew();
            status_.setText(
                "No Sound State declared | create a bounded exact Sound or SoundCollection variable",
                false);
        }
    }

    void refreshLiveValues() override
    {
        rebuildInstances();
        updateLivePresentation();
    }

    void focusSoundState() override
    {
        auto& target = selectedDeclaration_.has_value()
            ? static_cast<juce::Component&>(declarationBox_)
            : static_cast<juce::Component&>(newButton_);
        if (target.isShowing())
            target.grabKeyboardFocus();
    }

    bool selectDeclarationForSmoke(
        const talas::project::ProjectVariableOwner& owner,
        std::string_view variableId) override
    {
        const DeclarationIdentity identity{owner, std::string(variableId)};
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
        const talas::project::PreparedProjectSoundVariableIdentity& identity,
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

    void createDeclarationForSmoke(
        talas::project::ProjectSoundVariableDeclaration declaration) override
    {
        const auto identity = DeclarationIdentity{
            declaration.owner,
            declaration.variableId};
        const auto result = bridge_.editor().upsertProjectSoundVariable(
            std::move(declaration));
        if (result.ok())
        {
            selectedDeclaration_ = identity;
            onDocumentChange_();
        }
        else
        {
            onStatus_("Sound State create blocked: "
                + juceString(result.message));
        }
        refresh();
    }

    bool assignSelectionForSmoke(
        std::string_view definitionId) override
    {
        const auto found = std::ranges::find(
            liveCandidateIds_,
            definitionId);
        if (found == liveCandidateIds_.end())
            return false;
        liveSelectionBox_.setSelectedItemIndex(
            static_cast<int>(std::distance(liveCandidateIds_.begin(), found)),
            juce::dontSendNotification);
        return assignLive();
    }

    bool resetSelectionForSmoke() override { return resetSelected(); }
    bool captureSelectionForSmoke() override { return captureInitial(); }

    bool surfaceReadyForSmoke(
        std::string_view variableId,
        std::string_view selectedDefinitionId,
        bool assignmentEnabled) const override
    {
        return selectedDeclaration_.has_value()
            && selectedDeclaration_->variableId == variableId
            && declarationBox_.getComponentID().contains(
                framed(variableId))
            && liveValue_.getText().contains(
                selectedDefinitionId.empty()
                    ? juce::String{"silence"}
                    : juceString(selectedDefinitionId))
            && assignButton_.isEnabled() == assignmentEnabled;
    }

private:
    [[nodiscard]] std::optional<talas::project::ProjectVariableOwner>
    selectedOwner() const
    {
        const auto index = ownerBox_.getSelectedItemIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= ownerChoices_.size())
        {
            return std::nullopt;
        }
        return ownerChoices_[static_cast<std::size_t>(index)];
    }

    void rebuildOwners()
    {
        const auto retained = selectedOwner();
        ownerChoices_.clear();
        ownerBox_.clear(juce::dontSendNotification);
        ownerChoices_.push_back({});
        ownerBox_.addItem("Project", 1);
        ownerChoices_.push_back({
            talas::project::ProjectVariableOwnerKind::WorkspaceSession});
        ownerBox_.addItem("Workspace Session | local", 2);
        const auto appendVoiceOwners = [&] (
            const auto& graph,
            std::string_view definitionId,
            const juce::String& prefix) {
            for (const auto& node : graph.nodes)
            {
                if (node.typeId != "talas.midiVoice")
                    continue;
                ownerChoices_.push_back({
                    talas::project::ProjectVariableOwnerKind::MidiVoice,
                    std::string(definitionId),
                    node.instanceId});
                ownerBox_.addItem(
                    "MIDI Voice | " + prefix + " / "
                        + juceString(node.instanceId),
                    static_cast<int>(ownerChoices_.size()));
            }
        };
        appendVoiceOwners(
            bridge_.editor().document().graph,
            {},
            "Project");
        for (const auto& definition :
             bridge_.editor().document().soundDefinitions)
        {
            ownerChoices_.push_back({
                talas::project::ProjectVariableOwnerKind::SoundDefinition,
                definition.definitionId});
            ownerBox_.addItem(
                "Sound | " + definitionName(
                    bridge_.editor().document(),
                    definition.definitionId),
                static_cast<int>(ownerChoices_.size()));
            appendVoiceOwners(
                definition.graph,
                definition.definitionId,
                "Sound " + juceString(definition.definitionId));
        }
        if (retained.has_value())
        {
            const auto found = std::ranges::find(ownerChoices_, *retained);
            if (found != ownerChoices_.end())
                ownerBox_.setSelectedItemIndex(
                    static_cast<int>(
                        std::distance(ownerChoices_.begin(), found)),
                    juce::dontSendNotification);
        }
        if (ownerBox_.getSelectedId() == 0)
            ownerBox_.setSelectedId(1, juce::dontSendNotification);
    }

    void rebuildDefinitions()
    {
        definitionChoices_.clear();
        candidateDefinitionBox_.clear(juce::dontSendNotification);
        for (const auto& definition :
             bridge_.editor().document().soundDefinitions)
        {
            definitionChoices_.push_back(definition.definitionId);
            candidateDefinitionBox_.addItem(
                definitionName(
                    bridge_.editor().document(),
                    definition.definitionId),
                static_cast<int>(definitionChoices_.size()));
        }
        if (!definitionChoices_.empty())
            candidateDefinitionBox_.setSelectedItemIndex(
                0,
                juce::dontSendNotification);
        updateDefinitionChoiceFocusIdentity();
    }

    void selectDeclaration()
    {
        const auto index = declarationBox_.getSelectedItemIndex();
        if (index < 0
            || static_cast<std::size_t>(index)
                >= declarationChoices_.size())
        {
            return;
        }
        loadDeclaration(
            declarationChoices_[static_cast<std::size_t>(index)]);
    }

    void loadDeclaration(const DeclarationIdentity& identity)
    {
        const auto& variables =
            bridge_.editor().document().soundVariableLibrary.variables;
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
        kindBox_.setSelectedId(
            found->kind == talas::project::ProjectSoundVariableKind::Sound
                ? 1
                : 2,
            juce::dontSendNotification);
        const auto owner = std::ranges::find(ownerChoices_, found->owner);
        ownerBox_.setSelectedItemIndex(
            owner == ownerChoices_.end()
                ? 0
                : static_cast<int>(
                    std::distance(ownerChoices_.begin(), owner)),
            juce::dontSendNotification);
        snapshotEligible_.setToggleState(
            found->snapshotEligible,
            juce::dontSendNotification);
        snapshotEligible_.setEnabled(
            found->owner.kind
            != talas::project::ProjectVariableOwnerKind::MidiVoice);
        candidateIds_ = found->candidateDefinitionIds;
        initialDefinitionId_ = found->candidateDefinitionIds.empty()
            ? std::string{}
            : found->candidateDefinitionIds[
                found->initialSelectedOrdinal];
        rebuildCandidateEditors();
        const auto exact = ownerIdentity(found->owner) + ":"
            + framed(found->variableId);
        const auto prefix = "inspector.sound-state:" + exact + ":";
        declarationBox_.setComponentID(
            "inspector.sound-state.list:" + exact);
        variableId_.setComponentID(prefix + "id");
        ownerBox_.setComponentID(prefix + "owner");
        displayName_.setComponentID(prefix + "display-name");
        kindBox_.setComponentID(prefix + "kind");
        snapshotEligible_.setComponentID(prefix + "snapshot");
        saveButton_.setComponentID(prefix + "save");
        deleteButton_.setComponentID(prefix + "delete");
        saveButton_.setEnabled(true);
        deleteButton_.setEnabled(true);
        pendingEdit_.reset();
        pendingDelete_.reset();
        saveButton_.setButtonText("Save");
        deleteButton_.setButtonText("Delete");
        refreshLiveValues();
    }

    void beginNew()
    {
        selectedDeclaration_.reset();
        selectedPreparedChoice_.reset();
        variableId_.clear();
        displayName_.clear();
        ownerBox_.setSelectedId(1, juce::dontSendNotification);
        kindBox_.setSelectedId(1, juce::dontSendNotification);
        snapshotEligible_.setToggleState(true, juce::dontSendNotification);
        snapshotEligible_.setEnabled(true);
        candidateIds_.clear();
        initialDefinitionId_.clear();
        rebuildCandidateEditors();
        instanceBox_.clear(juce::dontSendNotification);
        liveValue_.setText("Unavailable | declaration not saved", false);
        liveSelectionBox_.clear(juce::dontSendNotification);
        saveButton_.setEnabled(true);
        deleteButton_.setEnabled(false);
        pendingEdit_.reset();
        pendingDelete_.reset();
        status_.setText(
            "New Sound State | choose exact owner, kind, candidates, and initial selection",
            false);
    }

    void rebuildCandidateEditors()
    {
        const auto selectedCandidate = candidateBox_.getSelectedItemIndex();
        candidateBox_.clear(juce::dontSendNotification);
        initialSelectionBox_.clear(juce::dontSendNotification);
        for (std::size_t index = 0; index < candidateIds_.size(); ++index)
        {
            const auto label = definitionName(
                bridge_.editor().document(),
                candidateIds_[index]);
            candidateBox_.addItem(label, static_cast<int>(index + 1));
            initialSelectionBox_.addItem(label, static_cast<int>(index + 1));
        }
        if (!candidateIds_.empty())
        {
            candidateBox_.setSelectedItemIndex(
                std::clamp(
                    selectedCandidate,
                    0,
                    static_cast<int>(candidateIds_.size() - 1)),
                juce::dontSendNotification);
            const auto initial = std::ranges::find(
                candidateIds_,
                initialDefinitionId_);
            if (initial == candidateIds_.end())
            {
                initialDefinitionId_ = candidateIds_.front();
                initialSelectionBox_.setSelectedItemIndex(
                    0,
                    juce::dontSendNotification);
            }
            else
            {
                initialSelectionBox_.setSelectedItemIndex(
                    static_cast<int>(
                        std::distance(candidateIds_.begin(), initial)),
                    juce::dontSendNotification);
            }
        }
        updateCandidateButtons();
        updateInitialFocusIdentity();
    }

    void updateCandidateButtons()
    {
        const auto index = candidateBox_.getSelectedItemIndex();
        const auto valid = index >= 0
            && static_cast<std::size_t>(index) < candidateIds_.size();
        removeCandidateButton_.setEnabled(valid);
        moveCandidateUpButton_.setEnabled(valid && index > 0);
        moveCandidateDownButton_.setEnabled(
            valid
            && static_cast<std::size_t>(index + 1) < candidateIds_.size());
        const auto candidate = valid
            ? candidateIds_[static_cast<std::size_t>(index)]
            : std::string{};
        const auto exact = selectedDeclaration_.has_value()
            ? framed(selectedDeclaration_->variableId) + ":"
                + framed(candidate)
            : juce::String{"new:"} + framed(candidate);
        candidateBox_.setComponentID(
            "inspector.sound-state.candidate:" + exact);
        removeCandidateButton_.setComponentID(
            "inspector.sound-state.remove-candidate:" + exact);
        moveCandidateUpButton_.setComponentID(
            "inspector.sound-state.move-candidate-up:" + exact);
        moveCandidateDownButton_.setComponentID(
            "inspector.sound-state.move-candidate-down:" + exact);
    }

    void updateDefinitionChoiceFocusIdentity()
    {
        const auto index = candidateDefinitionBox_.getSelectedItemIndex();
        const auto definitionId = index >= 0
                && static_cast<std::size_t>(index)
                    < definitionChoices_.size()
            ? definitionChoices_[static_cast<std::size_t>(index)]
            : std::string{};
        const auto variableId = selectedDeclaration_.has_value()
            ? selectedDeclaration_->variableId
            : variableId_.getText().trim().toStdString();
        const auto exact = framed(variableId) + ":"
            + framed(definitionId);
        candidateDefinitionBox_.setComponentID(
            "inspector.sound-state.add-candidate:" + exact);
        addCandidateButton_.setComponentID(
            "inspector.sound-state.add-candidate-action:" + exact);
    }

    void updateInitialFocusIdentity()
    {
        const auto variableId = selectedDeclaration_.has_value()
            ? selectedDeclaration_->variableId
            : variableId_.getText().trim().toStdString();
        initialSelectionBox_.setComponentID(
            "inspector.sound-state.initial:" + framed(variableId)
            + ":" + framed(initialDefinitionId_));
    }

    void updateLiveSelectionFocusIdentity()
    {
        const auto index = liveSelectionBox_.getSelectedItemIndex();
        const auto definitionId = index >= 0
                && static_cast<std::size_t>(index)
                    < liveCandidateIds_.size()
            ? liveCandidateIds_[static_cast<std::size_t>(index)]
            : std::string{};
        const auto variableId = selectedPreparedChoice_.has_value()
            ? selectedPreparedChoice_->identity.variableId
            : std::string{};
        liveSelectionBox_.setComponentID(
            "inspector.sound-state.live-candidate:"
            + framed(variableId) + ":" + framed(definitionId));
    }

    void addCandidate()
    {
        const auto index = candidateDefinitionBox_.getSelectedItemIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= definitionChoices_.size()
            || candidateIds_.size()
                >= talas::project::maximumProjectSoundVariableCandidates)
        {
            return;
        }
        const auto& definitionId =
            definitionChoices_[static_cast<std::size_t>(index)];
        if (std::ranges::find(candidateIds_, definitionId)
            != candidateIds_.end())
        {
            onStatus_("Sound candidate is already present");
            return;
        }
        candidateIds_.push_back(definitionId);
        if (initialDefinitionId_.empty())
            initialDefinitionId_ = definitionId;
        rebuildCandidateEditors();
        candidateBox_.setSelectedItemIndex(
            static_cast<int>(candidateIds_.size() - 1),
            juce::dontSendNotification);
        updateCandidateButtons();
    }

    void removeCandidate()
    {
        const auto index = candidateBox_.getSelectedItemIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= candidateIds_.size())
        {
            return;
        }
        const auto removed = candidateIds_[static_cast<std::size_t>(index)];
        candidateIds_.erase(candidateIds_.begin() + index);
        if (initialDefinitionId_ == removed)
            initialDefinitionId_ = candidateIds_.empty()
                ? std::string{}
                : candidateIds_[std::min<std::size_t>(
                    static_cast<std::size_t>(index),
                    candidateIds_.size() - 1)];
        rebuildCandidateEditors();
    }

    void moveCandidate(int direction)
    {
        const auto index = candidateBox_.getSelectedItemIndex();
        const auto target = index + direction;
        if (index < 0 || target < 0
            || static_cast<std::size_t>(index) >= candidateIds_.size()
            || static_cast<std::size_t>(target) >= candidateIds_.size())
        {
            return;
        }
        std::swap(
            candidateIds_[static_cast<std::size_t>(index)],
            candidateIds_[static_cast<std::size_t>(target)]);
        rebuildCandidateEditors();
        candidateBox_.setSelectedItemIndex(
            target,
            juce::dontSendNotification);
        updateCandidateButtons();
    }

    [[nodiscard]] std::optional<
        talas::project::ProjectSoundVariableDeclaration>
    editedDeclaration() const
    {
        const auto owner = selectedOwner();
        if (!owner.has_value())
            return std::nullopt;
        talas::project::ProjectSoundVariableDeclaration declaration;
        declaration.variableId = variableId_.getText().trim().toStdString();
        declaration.displayName = displayName_.getText().trim().toStdString();
        declaration.owner = *owner;
        declaration.kind = kindBox_.getSelectedId() == 2
            ? talas::project::ProjectSoundVariableKind::SoundCollection
            : talas::project::ProjectSoundVariableKind::Sound;
        declaration.candidateDefinitionIds = candidateIds_;
        const auto initialIndex = initialSelectionBox_.getSelectedItemIndex();
        if (!candidateIds_.empty()
            && (initialIndex < 0
                || static_cast<std::size_t>(initialIndex)
                    >= candidateIds_.size()))
        {
            return std::nullopt;
        }
        declaration.initialSelectedOrdinal = candidateIds_.empty()
            ? 0
            : static_cast<std::uint16_t>(initialIndex);
        declaration.snapshotEligible = owner->kind
                == talas::project::ProjectVariableOwnerKind::MidiVoice
            ? false
            : snapshotEligible_.getToggleState();
        return declaration;
    }

    void saveDeclaration()
    {
        const auto edited = editedDeclaration();
        if (!edited.has_value())
        {
            onStatus_("Sound State edit blocked: exact owner and initial candidate are required");
            return;
        }
        const DeclarationIdentity nextIdentity{
            edited->owner,
            edited->variableId};
        const auto& variables =
            bridge_.editor().document().soundVariableLibrary.variables;
        const auto current = selectedDeclaration_.has_value()
            ? std::ranges::find_if(
                variables,
                [&](const auto& declaration) {
                    return declaration.owner == selectedDeclaration_->owner
                        && declaration.variableId
                            == selectedDeclaration_->variableId;
                })
            : variables.end();
        if (current != variables.end() && *current != *edited
            && (!pendingEdit_.has_value()
                || *pendingEdit_ != *edited))
        {
            pendingEdit_ = *edited;
            saveButton_.setButtonText("Confirm");
            status_.setText(
                "Confirm declaration/candidate authority change | exact references: "
                    + referencePathsText(
                        bridge_.editor().document(),
                        *selectedDeclaration_),
                false);
            return;
        }
        const auto result = current == variables.end()
            ? bridge_.editor().upsertProjectSoundVariable(*edited)
            : bridge_.editor().replaceProjectSoundVariable(
                selectedDeclaration_->owner,
                selectedDeclaration_->variableId,
                *edited);
        if (!result.ok())
        {
            pendingEdit_.reset();
            saveButton_.setButtonText("Save");
            status_.setText(
                "Edit blocked | " + juceString(result.message),
                false);
            onStatus_("Sound State edit blocked: "
                + juceString(result.message));
            return;
        }
        selectedDeclaration_ = nextIdentity;
        onDocumentChange_();
        onStatus_("Sound State declaration saved as one Undo step | fresh staging required");
        refresh();
    }

    void deleteDeclaration()
    {
        if (!selectedDeclaration_.has_value())
            return;
        if (pendingDelete_ != selectedDeclaration_)
        {
            pendingDelete_ = selectedDeclaration_;
            deleteButton_.setButtonText("Confirm");
            status_.setText(
                "Confirm Sound State deletion | exact references: "
                    + referencePathsText(
                        bridge_.editor().document(),
                        *selectedDeclaration_),
                false);
            return;
        }
        const auto removed = *selectedDeclaration_;
        const auto removedIndex = std::ranges::find(
            declarationChoices_,
            removed) - declarationChoices_.begin();
        const auto result = bridge_.editor().removeProjectSoundVariable(
            removed.owner,
            removed.variableId);
        if (!result.ok())
        {
            pendingDelete_.reset();
            deleteButton_.setButtonText("Delete");
            onStatus_("Sound State delete blocked: "
                + juceString(result.message));
            return;
        }
        selectedDeclaration_.reset();
        const auto& remaining =
            bridge_.editor().document().soundVariableLibrary.variables;
        if (!remaining.empty())
        {
            const auto neighbor = std::min<std::size_t>(
                static_cast<std::size_t>(removedIndex),
                remaining.size() - 1);
            selectedDeclaration_ = DeclarationIdentity{
                remaining[neighbor].owner,
                remaining[neighbor].variableId};
        }
        onDocumentChange_();
        onStatus_("Sound State declaration deleted as one Undo step | fresh staging required");
        refresh();
    }

    void rebuildInstances()
    {
        const auto retained = selectedPreparedChoice_;
        preparedChoices_.clear();
        instanceBox_.clear(juce::dontSendNotification);
        latestValues_ = bridge_.preparedSoundVariableValues();
        if (!selectedDeclaration_.has_value()
            || !latestValues_.publicationValid)
        {
            selectedPreparedChoice_.reset();
            updatePreparedFocusIdentity();
            return;
        }
        const auto& variables =
            bridge_.editor().document().soundVariableLibrary.variables;
        const auto declaration = std::ranges::find_if(
            variables,
            [&](const auto& candidate) {
                return candidate.owner == selectedDeclaration_->owner
                    && candidate.variableId
                        == selectedDeclaration_->variableId;
            });
        if (declaration == variables.end())
            return;
        for (const auto& value : latestValues_.values)
        {
            if (!declarationMatches(*declaration, value.identity))
                continue;
            preparedChoices_.push_back({
                value.identity,
                value.voiceGeneration});
            auto label = preparedOwnerText(value.identity);
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
                        + " | "
                        + (value.voiceActive
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
            const auto found = std::ranges::find(
                preparedChoices_,
                *retained);
            if (found != preparedChoices_.end())
            {
                instanceBox_.setSelectedItemIndex(
                    static_cast<int>(std::distance(
                        preparedChoices_.begin(),
                        found)),
                    juce::dontSendNotification);
                selectedPreparedChoice_ = *found;
                updatePreparedFocusIdentity();
                return;
            }
            if (retained->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice)
            {
                selectedPreparedChoice_.reset();
                updatePreparedFocusIdentity();
                return;
            }
        }
        selectedPreparedChoice_ = preparedChoices_.empty()
            ? std::optional<PreparedChoice>{}
            : std::optional<PreparedChoice>{preparedChoices_.front()};
        if (selectedPreparedChoice_.has_value())
            instanceBox_.setSelectedItemIndex(
                0,
                juce::dontSendNotification);
        updatePreparedFocusIdentity();
    }

    void selectInstance()
    {
        const auto index = instanceBox_.getSelectedItemIndex();
        selectedPreparedChoice_ = index < 0
                || static_cast<std::size_t>(index)
                    >= preparedChoices_.size()
            ? std::optional<PreparedChoice>{}
            : std::optional<PreparedChoice>{
                preparedChoices_[static_cast<std::size_t>(index)]};
        updatePreparedFocusIdentity();
        updateLivePresentation();
    }

    void updatePreparedFocusIdentity()
    {
        if (!selectedPreparedChoice_.has_value())
        {
            instanceBox_.setComponentID(
                "inspector.sound-state.instance:unavailable");
            assignButton_.setComponentID(
                "inspector.sound-state.assign:unavailable");
            resetButton_.setComponentID(
                "inspector.sound-state.reset:unavailable");
            return;
        }
        const auto& choice = *selectedPreparedChoice_;
        auto exact = juceString(std::to_string(
            static_cast<int>(choice.identity.ownerKind)));
        exact += ":" + framed(choice.identity.definitionId)
            + ":" + framed(choice.identity.ownerNodeId);
        for (const auto& segment : choice.identity.instancePath)
            exact += ":" + framed(segment);
        exact += ":" + framed(choice.identity.variableId);
        if (choice.identity.voiceOrdinal.has_value())
            exact += ":slot"
                + juce::String(
                    static_cast<int>(*choice.identity.voiceOrdinal));
        exact += ":generation"
            + juce::String(static_cast<juce::int64>(
                choice.voiceGeneration));
        instanceBox_.setComponentID(
            "inspector.sound-state.instance:" + exact);
        assignButton_.setComponentID(
            "inspector.sound-state.assign:" + exact);
        resetButton_.setComponentID(
            "inspector.sound-state.reset:" + exact);
        describeControl(
            instanceBox_,
            "Prepared Sound State instance: "
                + preparedOwnerText(choice.identity),
            choice.identity.ownerKind
                    == talas::project::ProjectVariableOwnerKind::MidiVoice
                ? "Runtime-only exact slot generation. Selection never redirects after voice stealing or slot reuse."
                : "Prepared cell bound to the exact active stage and structural owner identity.");
    }

    void updateLivePresentation()
    {
        liveCandidateIds_.clear();
        liveSelectionBox_.clear(juce::dontSendNotification);
        const auto coherent = selectedPreparedChoice_.has_value()
            && latestValues_.publicationValid
            && latestValues_.stageId == bridge_.activeStageId();
        const auto value = coherent
            ? std::ranges::find(
                latestValues_.values,
                selectedPreparedChoice_->identity,
                &talas::runtime::PreparedSoundVariableValue::identity)
            : latestValues_.values.end();
        const auto exactVoice = value != latestValues_.values.end()
            && (value->identity.ownerKind
                    != talas::project::ProjectVariableOwnerKind::MidiVoice
                || (value->voiceGeneration != 0
                    && value->voiceGeneration
                        == selectedPreparedChoice_->voiceGeneration));
        const auto found = coherent
            && value != latestValues_.values.end()
            && exactVoice;
        const auto& variables =
            bridge_.editor().document().soundVariableLibrary.variables;
        const auto declaration = selectedDeclaration_.has_value()
            ? std::ranges::find_if(
                variables,
                [&](const auto& candidate) {
                    return candidate.owner == selectedDeclaration_->owner
                        && candidate.variableId
                            == selectedDeclaration_->variableId;
                })
            : variables.end();
        if (declaration != variables.end())
        {
            liveCandidateIds_ = declaration->candidateDefinitionIds;
            for (std::size_t index = 0;
                 index < liveCandidateIds_.size();
                 ++index)
            {
                liveSelectionBox_.addItem(
                    definitionName(
                        bridge_.editor().document(),
                        liveCandidateIds_[index]),
                    static_cast<int>(index + 1));
            }
        }
        assignButton_.setEnabled(found && !liveCandidateIds_.empty());
        resetButton_.setEnabled(found);
        captureButton_.setEnabled(
            found && !value->selectedDefinitionId.empty()
            && selectedPreparedChoice_->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::Project);
        resetAllButton_.setEnabled(
            latestValues_.publicationValid
            && latestValues_.stageId == bridge_.activeStageId()
            && std::ranges::any_of(
                latestValues_.values,
                [](const auto& candidate) {
                    return candidate.identity.ownerKind
                        != talas::project::ProjectVariableOwnerKind::MidiVoice;
                }));
        if (!found)
        {
            liveValue_.setText(
                "Unavailable | stale stage, instance, or voice generation",
                false);
            status_.setText(
                selectedDeclaration_.has_value()
                    ? "Declaration retained | no coherent active-stage Sound selection"
                    : "No Sound State selected",
                false);
            return;
        }
        const auto selected = std::ranges::find(
            liveCandidateIds_,
            value->selectedDefinitionId);
        if (selected != liveCandidateIds_.end())
            liveSelectionBox_.setSelectedItemIndex(
                static_cast<int>(
                    std::distance(liveCandidateIds_.begin(), selected)),
                juce::dontSendNotification);
        updateLiveSelectionFocusIdentity();
        liveValue_.setText(
            value->selectedDefinitionId.empty()
                ? juce::String{"Current silence | empty collection"}
                : "Current "
                    + definitionName(
                        bridge_.editor().document(),
                        value->selectedDefinitionId),
            false);
        if (declaration != variables.end())
        {
            const auto initial = declaration->candidateDefinitionIds.empty()
                ? juce::String{"silence"}
                : definitionName(
                    bridge_.editor().document(),
                    declaration->candidateDefinitionIds[
                        declaration->initialSelectedOrdinal]);
            status_.setText(
                juceString(declaration->variableId) + " | "
                    + ownerText(declaration->owner) + " | "
                    + (declaration->kind
                                == talas::project::ProjectSoundVariableKind::Sound
                            ? "Sound"
                            : "SoundCollection")
                    + " | "
                    + juce::String(static_cast<juce::int64>(
                        declaration->candidateDefinitionIds.size()))
                    + " candidates | initial " + initial
                    + " | current "
                    + (value->selectedDefinitionId.empty()
                            ? juce::String{"silence"}
                            : definitionName(
                                bridge_.editor().document(),
                                value->selectedDefinitionId))
                    + " | snapshots "
                    + (declaration->snapshotEligible
                            ? "eligible"
                            : "excluded")
                    + " | functional subset: preprepared mono branches, fixed 128-frame crossfade, membership edits require restaging",
                false);
        }
    }

    bool assignLive()
    {
        if (!selectedPreparedChoice_.has_value())
            return false;
        const auto index = liveSelectionBox_.getSelectedItemIndex();
        if (index < 0
            || static_cast<std::size_t>(index)
                >= liveCandidateIds_.size())
        {
            return false;
        }
        const auto accepted = selectedPreparedChoice_->identity.ownerKind
                == talas::project::ProjectVariableOwnerKind::MidiVoice
            ? bridge_.scheduleVoiceSoundVariableAssignment(
                selectedPreparedChoice_->identity,
                selectedPreparedChoice_->voiceGeneration,
                liveCandidateIds_[static_cast<std::size_t>(index)],
                0,
                bridge_.latestStageGeneration())
            : bridge_.scheduleSoundVariableAssignment(
                selectedPreparedChoice_->identity,
                liveCandidateIds_[static_cast<std::size_t>(index)],
                0,
                bridge_.latestStageGeneration());
        onStatus_(accepted
            ? "Sound selection queued for the exact active stage"
            : "Sound selection rejected: stage or instance authority is stale");
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
            const auto& variables =
                bridge_.editor().document().soundVariableLibrary.variables;
            const auto declaration = selectedDeclaration_.has_value()
                ? std::ranges::find_if(
                    variables,
                    [&](const auto& candidate) {
                        return candidate.owner == selectedDeclaration_->owner
                            && candidate.variableId
                                == selectedDeclaration_->variableId;
                    })
                : variables.end();
            if (declaration != variables.end()
                && declaration->candidateDefinitionIds.empty())
            {
                const auto publication = bridge_.preparedSoundVariableValues();
                const auto value = std::ranges::find_if(
                    publication.values,
                    [&](const auto& candidate) {
                        return candidate.identity
                                == selectedPreparedChoice_->identity
                            && candidate.voiceGeneration
                                == selectedPreparedChoice_->voiceGeneration;
                    });
                accepted = publication.publicationValid
                    && publication.stageId != 0
                    && publication.stageId
                        == bridge_.latestPreparedStageId()
                    && publication.stageId
                        == bridge_.activeStageId()
                    && value != publication.values.end()
                    && value->selectedDefinitionId.empty();
            }
            else
            {
                accepted = declaration != variables.end()
                    && bridge_.scheduleVoiceSoundVariableAssignment(
                        selectedPreparedChoice_->identity,
                        selectedPreparedChoice_->voiceGeneration,
                        declaration->candidateDefinitionIds[
                            declaration->initialSelectedOrdinal],
                        0,
                        bridge_.latestStageGeneration());
            }
        }
        else
        {
            const std::array identities{
                selectedPreparedChoice_->identity};
            accepted = bridge_.resetSoundVariables(
                    identities,
                    bridge_.latestStageGeneration())
                == talas::runtime::SnapshotDirectTransactionResult::Accepted;
        }
        onStatus_(accepted
            ? "Selected Sound State reset to its exact initial selection"
            : "Sound State reset rejected: stage or instance authority is stale");
        if (accepted)
            onLiveMutation_();
        return accepted;
    }

    void resetAll()
    {
        const auto result = bridge_.resetAllSoundVariables(
            bridge_.latestStageGeneration());
        if (result
            == talas::runtime::SnapshotDirectTransactionResult::Accepted)
        {
            onLiveMutation_();
        }
        onStatus_(result
                    == talas::runtime::SnapshotDirectTransactionResult::Accepted
                ? "All shared prepared Sound State cells reset in one transaction"
                : "Reset shared Sound State rejected: stale stage or transaction authority");
    }

    bool captureInitial()
    {
        if (!selectedPreparedChoice_.has_value())
            return false;
        const auto result =
            bridge_.captureProjectSoundVariableInitialSelection(
                selectedPreparedChoice_->identity,
                bridge_.latestStageGeneration(),
                bridge_.editor().documentAuthorityGeneration());
        if (!result.ok())
        {
            onStatus_("Sound State capture blocked: "
                + juceString(result.message));
            return false;
        }
        onDocumentChange_();
        onStatus_("Captured current project Sound selection as initial | fresh staging required");
        refresh();
        return true;
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    DocumentChangeCallback onDocumentChange_;
    LiveMutationCallback onLiveMutation_;
    StatusCallback onStatus_;
    std::vector<DeclarationIdentity> declarationChoices_;
    std::vector<talas::project::ProjectVariableOwner> ownerChoices_;
    std::vector<std::string> definitionChoices_;
    std::vector<std::string> candidateIds_;
    std::string initialDefinitionId_;
    std::vector<PreparedChoice> preparedChoices_;
    std::vector<std::string> liveCandidateIds_;
    std::optional<DeclarationIdentity> selectedDeclaration_;
    std::optional<DeclarationIdentity> pendingDelete_;
    std::optional<talas::project::ProjectSoundVariableDeclaration> pendingEdit_;
    std::optional<PreparedChoice> selectedPreparedChoice_;
    talas::runtime::PreparedSoundVariableValueSnapshot latestValues_;
    juce::ComboBox declarationBox_;
    juce::TextButton newButton_;
    juce::TextEditor variableId_;
    juce::ComboBox ownerBox_;
    juce::TextEditor displayName_;
    juce::ComboBox kindBox_;
    juce::ToggleButton snapshotEligible_;
    juce::ComboBox candidateBox_;
    juce::ComboBox candidateDefinitionBox_;
    juce::TextButton addCandidateButton_;
    juce::TextButton removeCandidateButton_;
    juce::TextButton moveCandidateUpButton_;
    juce::TextButton moveCandidateDownButton_;
    juce::ComboBox initialSelectionBox_;
    juce::TextButton saveButton_;
    juce::TextButton deleteButton_;
    juce::ComboBox instanceBox_;
    juce::TextEditor liveValue_;
    juce::ComboBox liveSelectionBox_;
    juce::TextButton assignButton_;
    juce::TextButton resetButton_;
    juce::TextButton resetAllButton_;
    juce::TextButton captureButton_;
    juce::TextEditor status_;
};
}

std::unique_ptr<SoundStatePanel> makeSoundStatePanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    SoundStatePanel::DocumentChangeCallback onDocumentChange,
    SoundStatePanel::LiveMutationCallback onLiveMutation,
    SoundStatePanel::StatusCallback onStatus)
{
    return std::make_unique<SoundStatePanelImpl>(
        bridge,
        std::move(onDocumentChange),
        std::move(onLiveMutation),
        std::move(onStatus));
}
}
