#include "app/workbench/WorkbenchComponentInternal.h"

#include <fstream>
#include <numbers>

namespace talas::app::workbench
{
namespace
{
[[nodiscard]] talas::project::ProjectParameterExpression expressionValue(
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = "value";
    parameter.expression = std::move(expression);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression expressionParameterForSmoke(
    std::string parameterId,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(parameterId);
    parameter.expression = std::move(expression);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression textParameterForSmoke(
    std::string parameterId,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(parameterId);
    parameter.text = std::move(value);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression resourceParameterForSmoke(
    std::string parameterId,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(parameterId);
    parameter.resource = std::move(value);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression booleanParameterForSmoke(
    std::string parameterId,
    bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(parameterId);
    parameter.boolean = value;
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression choiceParameterForSmoke(
    std::string parameterId,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(parameterId);
    parameter.choice = std::move(value);
    return parameter;
}

[[nodiscard]] talas::project::ProjectNode midiVoiceForSmoke(
    std::string nodeId,
    int channel)
{
    return {
        std::move(nodeId),
        "talas.midiVoice",
        {
            expressionParameterForSmoke("left", "1"),
            expressionParameterForSmoke("right", "1"),
            expressionParameterForSmoke("channel", std::to_string(channel)),
            textParameterForSmoke("source", ""),
            resourceParameterForSmoke("midiFile", ""),
            expressionParameterForSmoke("polyphony", "1"),
            expressionParameterForSmoke("lowPitch", "0 nn"),
            expressionParameterForSmoke("highPitch", "127 nn"),
            textParameterForSmoke("script", ""),
            booleanParameterForSmoke("shared", false),
        },
        {},
    };
}

[[nodiscard]] talas::project::ProjectNode midiMapperForSmoke(
    std::string mode)
{
    return {
        "mapper",
        "talas.midiMapper",
        {
            textParameterForSmoke("map", std::move(mode)),
            choiceParameterForSmoke("voiceModel", "legacyPassthrough"),
            expressionParameterForSmoke("left", "1"),
            expressionParameterForSmoke("right", "1"),
            expressionParameterForSmoke("channel", "0"),
            textParameterForSmoke("source", ""),
            resourceParameterForSmoke("midiFile", ""),
            expressionParameterForSmoke("polyphony", "4"),
            expressionParameterForSmoke("lowPitch", "0 nn"),
            expressionParameterForSmoke("highPitch", "127 nn"),
            textParameterForSmoke("script", ""),
            booleanParameterForSmoke("shared", false),
        },
        {},
    };
}

[[nodiscard]] talas::project::ProjectDocument midiMapperDocumentForSmoke(
    std::string mode)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        midiMapperForSmoke(std::move(mode)),
        {"osc", "talas.noise", {
            expressionParameterForSmoke("initialState", "1"),
            expressionParameterForSmoke("frequency", "0 hz"),
            expressionParameterForSmoke("centerValue", "1"),
            expressionParameterForSmoke("scale", "0"),
            choiceParameterForSmoke("type", "hotPink"),
        }, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
    };
    return document;
}

[[nodiscard]] talas::project::ProjectNode soundInstanceForSmoke(
    std::string instanceId,
    std::string definitionId)
{
    talas::project::ProjectNode node;
    node.instanceId = std::move(instanceId);
    node.soundDefinitionId = std::move(definitionId);
    return node;
}
}

std::optional<std::string> WorkbenchComponent::runExpressionLanguageSmoke(
    const std::filesystem::path& smokeDirectory)
{
    auto require = [](bool condition, std::string message)
        -> std::optional<std::string> {
        if (!condition)
            return message;
        return {};
    };
    auto edit = [](const talas::project::EditResult& result, std::string action)
        -> std::optional<std::string> {
        if (result.ok())
            return {};
        return action + ": " + result.message;
    };

    talas::project::ProjectDocument soundStateDocument;
    soundStateDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    const auto addToneDefinition = [&soundStateDocument](
        std::string definitionId,
        std::string displayName,
        double amplitude) {
        talas::project::ProjectSoundDefinition definition;
        definition.definitionId = std::move(definitionId);
        definition.displayName = std::move(displayName);
        definition.outputs.push_back({
            "audioOut",
            talas::core::PortType::Audio,
            "osc",
            "audioOut"});
        definition.graph.nodes.push_back({
            "osc",
            "talas.oscillator",
            {
                expressionParameterForSmoke("frequency", "220 hz"),
                expressionParameterForSmoke(
                    "amplitude",
                    std::to_string(amplitude)),
            },
            {},
        });
        soundStateDocument.soundDefinitions.push_back(
            std::move(definition));
    };
    addToneDefinition("stateToneA", "State Tone A", 0.1);
    addToneDefinition("stateToneB", "State Tone B", 0.2);
    talas::project::ProjectParameterExpression selectorVariableId;
    selectorVariableId.parameterId = "variableId";
    selectorVariableId.text = "soundChoice";
    talas::project::ProjectParameterExpression selectorMembers;
    selectorMembers.parameterId = "members";
    selectorMembers.choice = "legacySelected";
    soundStateDocument.graph.nodes = {
        {"selector", "talas.soundCollectionVariable",
            {std::move(selectorVariableId), std::move(selectorMembers)}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    soundStateDocument.graph.connections = {
        {"selector", "audioOut", "output", "audioIn"},
    };
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(soundStateDocument),
            "Sound State Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    talas::project::ProjectSoundVariableDeclaration soundChoice;
    soundChoice.variableId = "soundChoice";
    soundChoice.displayName = "Sound choice";
    soundChoice.kind =
        talas::project::ProjectSoundVariableKind::SoundCollection;
    soundChoice.candidateDefinitionIds = {
        "stateToneA",
        "stateToneB",
    };
    inspectorPanel_->createSoundVariableForSmoke(soundChoice);
    const talas::project::PreparedProjectSoundVariableIdentity
        soundChoiceIdentity{
            talas::project::ProjectVariableOwnerKind::Project,
            "soundChoice",
        };
    talas::runtime::RenderSettings soundStateSettings;
    soundStateSettings.sampleRate = 48'000.0;
    soundStateSettings.maxBlockFrames = 8;
    projectBridge_.setPlaying(true);
    const auto renderSoundState = [&] {
        std::array<float, 8> output{};
        std::array<float*, 1> channels{output.data()};
        return projectBridge_.render(
            channels.data(),
            channels.size(),
            output.size());
    };
    if (auto error = require(
            projectBridge_.compileAndStage(soundStateSettings).ok()
                && renderSoundState()
                && inspectorPanel_->selectSoundVariableForSmoke(
                    soundChoice.owner,
                    soundChoice.variableId)
                && inspectorPanel_->selectPreparedSoundValueForSmoke(
                    soundChoiceIdentity,
                    0)
                && inspectorPanel_->soundStateReadyForSmoke(
                    "soundChoice",
                    "stateToneA",
                    true),
            "Sound State Inspector did not expose the exact initial prepared selection"))
    {
        return error;
    }
    const auto firstSoundStage = projectBridge_.latestStageGeneration();
    if (auto error = require(
            inspectorPanel_->assignSelectedSoundForSmoke("stateToneB")
                && renderSoundState(),
            "Sound State Inspector did not queue the exact candidate selection"))
    {
        return error;
    }
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->soundStateReadyForSmoke(
                "soundChoice",
                "stateToneB",
                true)
                && projectBridge_.compileAndStage(soundStateSettings).ok()
                && renderSoundState()
                && inspectorPanel_->selectSoundVariableForSmoke(
                    soundChoice.owner,
                    soundChoice.variableId)
                && inspectorPanel_->selectPreparedSoundValueForSmoke(
                    soundChoiceIdentity,
                    0)
                && inspectorPanel_->captureSelectedSoundForSmoke()
                && projectBridge_.editor().document()
                       .soundVariableLibrary.variables.front()
                       .initialSelectedOrdinal
                    == 1,
            "Sound State compatible restage/capture did not retain exact definition identity"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.compileAndStage(soundStateSettings).ok()
                && renderSoundState()
                && inspectorPanel_->selectSoundVariableForSmoke(
                    soundChoice.owner,
                    soundChoice.variableId)
                && inspectorPanel_->selectPreparedSoundValueForSmoke(
                    soundChoiceIdentity,
                    0)
                && inspectorPanel_->assignSelectedSoundForSmoke("stateToneA")
                && renderSoundState()
                && inspectorPanel_->resetSelectedSoundForSmoke()
                && renderSoundState()
                && !projectBridge_.scheduleSoundVariableAssignment(
                    soundChoiceIdentity,
                    "stateToneA",
                    0,
                    firstSoundStage),
            "Sound State reset or stale-stage rejection did not preserve exact authority"))
    {
        return error;
    }
    talas::project::ProjectSoundVariableDeclaration workspaceSoundChoice;
    workspaceSoundChoice.variableId = "workspaceSoundChoice";
    workspaceSoundChoice.displayName = "Workspace Sound choice";
    workspaceSoundChoice.owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    workspaceSoundChoice.kind =
        talas::project::ProjectSoundVariableKind::SoundCollection;
    workspaceSoundChoice.candidateDefinitionIds = {
        "stateToneA",
        "stateToneB",
    };
    inspectorPanel_->createSoundVariableForSmoke(workspaceSoundChoice);
    const talas::project::PreparedProjectSoundVariableIdentity
        workspaceSoundChoiceIdentity{
            talas::project::ProjectVariableOwnerKind::WorkspaceSession,
            "workspaceSoundChoice",
        };
    if (auto error = require(
            projectBridge_.compileAndStage(soundStateSettings).ok()
                && renderSoundState()
                && inspectorPanel_->selectSoundVariableForSmoke(
                    workspaceSoundChoice.owner,
                    workspaceSoundChoice.variableId)
                && inspectorPanel_->selectPreparedSoundValueForSmoke(
                    workspaceSoundChoiceIdentity,
                    0)
                && inspectorPanel_->assignSelectedSoundForSmoke("stateToneB")
                && renderSoundState(),
            "Workspace Sound State did not stage and acknowledge one exact selection"))
    {
        return error;
    }
    currentDocumentStaged_ = true;
    synchronizeWorkspaceSessionSoundVariableValues();
    if (auto error = require(
            workspaceSession_.soundVariableValues.size() == 1
                && workspaceSession_.soundVariableValues.front().variableId
                    == "workspaceSoundChoice"
                && workspaceSession_.soundVariableValues.front()
                       .selectedDefinitionId
                    == "stateToneB",
            "Workspace Sound State did not persist only callback-acknowledged exact identity"))
    {
        return error;
    }
    inspectorPanel_->showSoundState();
    inspectorPanel_->refreshVariableValues();
    resized();
    const auto soundStateImage = createComponentSnapshot(
        getLocalBounds(),
        true,
        1.0F);
    if (!soundStateImage.isValid())
        return "Workbench Sound State snapshot did not produce a valid image";
    auto soundStateStream = juce::File{juceString(
        (smokeDirectory / "workbench-sound-state.png")
            .lexically_normal()
            .string())}
                                .createOutputStream();
    if (soundStateStream == nullptr || !soundStateStream->openedOk())
        return "Workbench Sound State snapshot output could not be opened";
    if (!soundStateStream->setPosition(0)
        || soundStateStream->truncate().failed()
        || !juce::PNGImageFormat{}.writeImageToStream(
            soundStateImage,
            *soundStateStream))
    {
        return "Workbench Sound State snapshot could not be replaced";
    }
    soundStateStream->flush();

    newProjectNow();
    if (auto error = require(
            projectBridge_.editor().document().graph.nodes.empty()
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV2
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v2", true, true)
                && !projectPath().has_value()
                && !editBoundary_.isDirty(projectBridge_.editor().document()),
            "new-project state was not clean, empty, and Variables v2"))
    {
        return error;
    }

    talas::project::ProjectDocument soundVariableDocument;
    soundVariableDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    talas::project::ProjectSoundDefinition voiceDefinition;
    voiceDefinition.definitionId = "variableVoice";
    voiceDefinition.displayName = "Variable Voice";
    voiceDefinition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"});
    voiceDefinition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expressionParameterForSmoke("frequency", "240 hz"),
            expressionParameterForSmoke("amplitude", "0.25"),
        },
        {},
    });
    soundVariableDocument.soundDefinitions.push_back(
        std::move(voiceDefinition));
    soundVariableDocument.graph.nodes = {
        soundInstanceForSmoke("voiceA", "variableVoice"),
        soundInstanceForSmoke("voiceB", "variableVoice"),
        {"mix", "talas.mixer", {}, {}},
        {"output", "talas.audioOutput", {}, {}},
    };
    soundVariableDocument.graph.connections = {
        {"voiceA", "audioOut", "mix", "in1"},
        {"voiceB", "audioOut", "mix", "in2"},
        {"mix", "audioOut", "output", "audioIn"},
    };
    talas::project::ProjectSnapshotBank voiceBank;
    voiceBank.bankId = "voice-a-variables";
    voiceBank.displayName = "Voice A Variables";
    voiceBank.scope.kind = talas::project::SnapshotScopeKind::Node;
    voiceBank.scope.nodeId = "voiceA";
    talas::project::ProjectSnapshot voiceSnapshot;
    voiceSnapshot.snapshotId = "raised";
    voiceSnapshot.displayName = "Raised";
    talas::project::SnapshotValueEntry voiceSnapshotValue;
    voiceSnapshotValue.target.kind =
        talas::project::SnapshotTargetKind::Variable;
    voiceSnapshotValue.target.instancePath = {"voiceA"};
    voiceSnapshotValue.target.definitionId = "variableVoice";
    voiceSnapshotValue.target.variableId = "pitch";
    voiceSnapshotValue.value.kind = talas::project::SnapshotValueKind::Number;
    voiceSnapshotValue.value.domain = talas::expr::ValueDomain::Hertz;
    voiceSnapshotValue.value.number = 300.0;
    voiceSnapshot.values.push_back(std::move(voiceSnapshotValue));
    voiceBank.snapshots.push_back(std::move(voiceSnapshot));
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(soundVariableDocument),
            "Sound-instance variable Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    talas::project::ProjectVariableDeclaration soundPitch;
    soundPitch.variableId = "pitch";
    soundPitch.displayName = "Instance pitch";
    soundPitch.domain = talas::expr::ValueDomain::Hertz;
    soundPitch.initialValue = 240.0;
    soundPitch.owner = {
        talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "variableVoice",
    };
    inspectorPanel_->createVariableForSmoke(soundPitch);
    if (auto error = edit(
            projectBridge_.editor().addSnapshotBank(
                std::move(voiceBank)),
            "Sound-instance variable snapshot authoring failed"))
    {
        return error;
    }
    auto authoredVoice =
        projectBridge_.editor().document().soundDefinitions.front();
    authoredVoice.graph.nodes.front().parameters.front().expression =
        "@pitch";
    if (auto error = edit(
            projectBridge_.editor().replaceSoundDefinition(
                "variableVoice", std::move(authoredVoice)),
            "Sound-instance variable expression authoring failed"))
    {
        return error;
    }
    documentChanged();
    talas::runtime::RenderSettings variableSettings;
    variableSettings.sampleRate = 48'000.0;
    variableSettings.maxBlockFrames = 8;
    variableSettings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    projectBridge_.setPlaying(true);
    std::array<float, 8> variableOutput{};
    std::array<float*, 1> variableChannels{variableOutput.data()};
    const auto renderVariableStage = [&] {
        return projectBridge_.render(
            variableChannels.data(),
            variableChannels.size(),
            variableOutput.size());
    };
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderVariableStage(),
            "two-instance variable Workbench smoke could not stage"))
    {
        return error;
    }
    const talas::project::PreparedProjectVariableIdentity voiceAIdentity{
        talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "pitch",
        "variableVoice",
        {"voiceA"},
    };
    const talas::project::PreparedProjectVariableIdentity voiceBIdentity{
        talas::project::ProjectVariableOwnerKind::SoundDefinition,
        "pitch",
        "variableVoice",
        {"voiceB"},
    };
    const auto preparedValue = [&](const auto& identity)
        -> std::optional<double> {
        const auto values = projectBridge_.preparedVariableValues();
        const auto found = std::ranges::find(
            values.values,
            identity,
            &talas::runtime::PreparedVariableValue::identity);
        return values.publicationValid && found != values.values.end()
            ? std::optional<double>{found->value.value}
            : std::nullopt;
    };
    inspectorPanel_->showVariables();
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->assignSelectedVariableForSmoke(180.0)
                && renderVariableStage()
                && preparedValue(voiceAIdentity) == 180.0
                && preparedValue(voiceBIdentity) == 240.0,
            "Variables Inspector did not assign one exact Sound instance independently"))
    {
        return error;
    }
    const auto capturedVoice = projectBridge_.captureSnapshotPerformance(
        "voice-a-variables", "raised");
    if (auto error = require(
            capturedVoice.ok()
                && projectBridge_.editor().document()
                       .snapshotLibrary.banks.front()
                       .snapshots.front().values.front().value.number
                    == 180.0,
            "snapshot capture did not store the coherent live Sound-instance variable"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderVariableStage()
                && preparedValue(voiceAIdentity) == 180.0
                && preparedValue(voiceBIdentity) == 240.0,
            "compatible restaging did not retain exact Sound-instance variable cells"))
    {
        return error;
    }
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->assignSelectedVariableForSmoke(300.0)
                && renderVariableStage()
                && preparedValue(voiceAIdentity) == 300.0
                && preparedValue(voiceBIdentity) == 240.0,
            "snapshot recall fixture could not diverge one exact Sound-instance variable"))
    {
        return error;
    }
    const auto recalled = projectBridge_.recallSnapshot(
        "voice-a-variables", "raised");
    if (auto error = require(
            recalled.ok()
                && renderVariableStage()
                && preparedValue(voiceAIdentity) == 180.0
                && preparedValue(voiceBIdentity) == 240.0,
            "snapshot recall did not update only its exact Sound-instance variable"))
    {
        return error;
    }
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->resetSelectedVariableForSmoke()
                && renderVariableStage()
                && preparedValue(voiceAIdentity) == 240.0
                && preparedValue(voiceBIdentity) == 240.0,
            "Variables Inspector reset did not restore the selected Sound instance"))
    {
        return error;
    }

    talas::project::ProjectDocument voiceVariableDocument;
    voiceVariableDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    voiceVariableDocument.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mix", "talas.mixer", {}},
        midiVoiceForSmoke("midiA", 1),
        midiVoiceForSmoke("midiB", 2),
        {"oscA", "talas.oscillator", {
            expressionParameterForSmoke("frequency", "!Pitch"),
            expressionParameterForSmoke("amplitude", "0.25"),
        }},
        {"oscB", "talas.oscillator", {
            expressionParameterForSmoke("frequency", "!Pitch"),
            expressionParameterForSmoke("amplitude", "0.5"),
        }},
    };
    voiceVariableDocument.graph.connections = {
        {"oscA", "audioOut", "midiA", "audioIn"},
        {"oscB", "audioOut", "midiB", "audioIn"},
        {"midiA", "leftOut", "mix", "in1"},
        {"midiB", "leftOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
    };
    talas::project::ProjectVariableDeclaration gainA;
    gainA.variableId = "gainA";
    gainA.displayName = "Voice A gain";
    gainA.initialValue = 0.25;
    gainA.snapshotEligible = false;
    gainA.owner = {
        talas::project::ProjectVariableOwnerKind::MidiVoice,
        {},
        "midiA",
    };
    auto gainB = gainA;
    gainB.variableId = "gainB";
    gainB.displayName = "Voice B gain";
    gainB.initialValue = 0.5;
    gainB.owner.nodeId = "midiB";
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                std::move(voiceVariableDocument)),
            "MIDI Voice variable Workbench smoke setup failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().upsertProjectVariable(gainA),
            "MIDI Voice A declaration authoring failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().upsertProjectVariable(gainB),
            "MIDI Voice B declaration authoring failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().setParameter(
                "oscA",
                expressionParameterForSmoke("amplitude", "@gainA")),
            "MIDI Voice A consumer expression authoring failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().setParameter(
                "oscB",
                expressionParameterForSmoke("amplitude", "@gainB")),
            "MIDI Voice B consumer expression authoring failed"))
    {
        return error;
    }
    documentChanged();
    const auto renderVoiceStage = [&](double callbackTimeSeconds) {
        return projectBridge_.render(
            {},
            callbackTimeSeconds,
            variableChannels.data(),
            variableChannels.size(),
            variableOutput.size());
    };
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderVoiceStage(0.0)
                && projectBridge_.enqueueMidiNoteOn(1, 60, 100, 1.0)
                && projectBridge_.enqueueMidiNoteOn(2, 67, 110, 1.0)
                && renderVoiceStage(2.0),
            "two-owner MIDI Voice variable fixture did not stage and allocate exact slots"))
    {
        return error;
    }
    const talas::project::PreparedProjectVariableIdentity gainAIdentity{
        talas::project::ProjectVariableOwnerKind::MidiVoice,
        "gainA",
        {},
        {},
        "midiA",
        std::uint16_t{0},
    };
    const talas::project::PreparedProjectVariableIdentity gainBIdentity{
        talas::project::ProjectVariableOwnerKind::MidiVoice,
        "gainB",
        {},
        {},
        "midiB",
        std::uint16_t{0},
    };
    const auto preparedVoiceValue = [&](const auto& identity)
        -> std::optional<talas::runtime::PreparedVariableValue> {
        const auto values = projectBridge_.preparedVariableValues();
        const auto found = std::ranges::find(
            values.values,
            identity,
            &talas::runtime::PreparedVariableValue::identity);
        return values.publicationValid && found != values.values.end()
            ? std::optional<talas::runtime::PreparedVariableValue>{*found}
            : std::nullopt;
    };
    const auto activeA = preparedVoiceValue(gainAIdentity);
    const auto activeB = preparedVoiceValue(gainBIdentity);
    inspectorPanel_->showVariables();
    if (auto error = require(
            activeA.has_value()
                && activeB.has_value()
                && activeA->voiceGeneration != 0
                && activeB->voiceGeneration != 0
                && inspectorPanel_->selectVariableForSmoke(
                    gainA.owner, gainA.variableId)
                && inspectorPanel_->selectPreparedVariableForSmoke(
                    gainAIdentity, activeA->voiceGeneration)
                && inspectorPanel_->preparedVariableReadyForSmoke(
                    "MIDI Voice midiA / slot 1 | generation",
                    "runtime-only exact voice generation",
                    true,
                    false)
                && inspectorPanel_->assignSelectedVariableForSmoke(0.75)
                && renderVoiceStage(2.25)
                && preparedVoiceValue(gainAIdentity)->value.value == 0.75
                && preparedVoiceValue(gainBIdentity)->value.value == 0.5,
            "Variables Inspector did not expose and independently assign two exact MIDI Voice owners"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.enqueueMidiNoteOff(1, 60, 3.0)
                && renderVoiceStage(4.0),
            "MIDI Voice release fixture did not enter its release tail"))
    {
        return error;
    }
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->selectPreparedVariableForSmoke(
                gainAIdentity, activeA->voiceGeneration)
                && inspectorPanel_->preparedVariableReadyForSmoke(
                    "| release",
                    "runtime-only exact voice generation",
                    true,
                    false)
                && projectBridge_.enqueueMidiNoteOn(1, 61, 120, 5.0)
                && renderVoiceStage(6.0),
            "MIDI Voice release row did not retain its exact live generation"))
    {
        return error;
    }
    for (std::size_t callback = 0; callback < 40; ++callback)
    {
        if (auto error = require(
                renderVoiceStage(
                    6.0 + static_cast<double>(callback + 1U) * 0.001),
                "MIDI Voice replacement delayed attack did not remain renderable"))
        {
            return error;
        }
    }
    const auto reusedA = preparedVoiceValue(gainAIdentity);
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            reusedA.has_value()
                && reusedA->voiceGeneration != activeA->voiceGeneration
                && reusedA->value.value == 0.25,
            "MIDI Voice slot reuse did not advance generation and reset its exact cells"))
    {
        return error;
    }
    if (auto error = require(
            !inspectorPanel_->selectPreparedVariableForSmoke(
                gainAIdentity, activeA->voiceGeneration),
            "MIDI Voice row silently rebound a stale slot generation"))
    {
        return error;
    }
    if (auto error = require(
            inspectorPanel_->selectPreparedVariableForSmoke(
                gainAIdentity, reusedA->voiceGeneration),
            "MIDI Voice row did not expose the replacement slot generation"))
    {
        return error;
    }
    if (auto error = require(
            inspectorPanel_->preparedVariableReadyForSmoke(
                "note 61 velocity 120 | active",
                "runtime-only exact voice generation",
                true,
                false),
            "MIDI Voice replacement row did not expose exact note, velocity, actions, and runtime-only status: "
                + inspectorPanel_->preparedVariableDescriptionForSmoke()))
    {
        return error;
    }

    auto variableDocument = makeDemoDocument();
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(variableDocument),
            "variable Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    talas::project::ProjectVariableDeclaration pitch;
    pitch.variableId = "pitch";
    pitch.displayName = "Project pitch";
    pitch.domain = talas::expr::ValueDomain::Hertz;
    pitch.initialValue = 240.0;
    const talas::project::PreparedProjectVariableIdentity projectPitchIdentity{
        talas::project::ProjectVariableOwnerKind::Project,
        "pitch",
        {},
        {},
    };
    inspectorPanel_->createVariableForSmoke(pitch);
    inspectorPanel_->commitExpressionForSmoke(
        "osc", "frequency", "@pitch");
    const auto variableStage =
        projectBridge_.compileAndStage(variableSettings);
    if (auto error = require(
            variableStage.ok()
                && renderVariableStage(),
            "variable Workbench smoke could not stage its exact project cell"))
    {
        return error;
    }
    inspectorPanel_->showVariables();
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->variableSurfaceReadyForSmoke(
                "pitch", "at initial")
                && inspectorPanel_->assignSelectedVariableForSmoke(120.0)
                && renderVariableStage(),
            "Variables Inspector did not expose or assign one exact project-owned live cell"))
    {
        return error;
    }
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->variableSurfaceReadyForSmoke(
                "pitch", "modified")
                && inspectorPanel_->resetSelectedVariableForSmoke()
                && renderVariableStage()
                && inspectorPanel_->assignSelectedVariableForSmoke(180.0)
                && renderVariableStage()
                && inspectorPanel_->captureSelectedVariableForSmoke()
                && projectBridge_.editor().document()
                       .variableLibrary.variables.front().initialValue
                    == 180.0,
            "Variables Inspector reset/capture actions did not preserve exact live and document authority"))
    {
        return error;
    }

    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderVariableStage(),
            "captured project variable did not restage"))
    {
        return error;
    }
    inspectorPanel_->showVariables();
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->assignSelectedVariableForSmoke(90.0)
                && renderVariableStage()
                && preparedValue(projectPitchIdentity) == 90.0,
            "project variable reload fixture did not diverge its live value"))
    {
        return error;
    }
    const auto persistedVariableBytes =
        talas::project::serializeProjectDocument(
            projectBridge_.editor().document());
    auto reloadedVariable =
        talas::project::parseProjectDocument(persistedVariableBytes);
    projectBridge_.resetVariableValueHistory();
    if (auto error = require(
            reloadedVariable.ok()
                && projectBridge_.editor()
                       .replaceDocument(std::move(reloadedVariable.document))
                       .ok(),
            "captured variable project did not round-trip for reload"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderVariableStage()
                && preparedValue(projectPitchIdentity) == 180.0,
            "reload did not restart the project variable from its captured declared value"))
    {
        return error;
    }

    auto workspaceDocument = makeDemoDocument();
    talas::project::ProjectVariableDeclaration localPitch;
    localPitch.variableId = "localPitch";
    localPitch.displayName = "Local pitch";
    localPitch.domain = talas::expr::ValueDomain::Hertz;
    localPitch.initialValue = 240.0;
    localPitch.owner = {
        talas::project::ProjectVariableOwnerKind::WorkspaceSession};
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(workspaceDocument),
            "workspace variable Workbench smoke setup failed"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().upsertProjectVariable(localPitch),
            "workspace variable declaration authoring failed"))
    {
        return error;
    }
    auto localGraph = projectBridge_.editor().document();
    const auto localOscillator = std::ranges::find(
        localGraph.graph.nodes,
        std::string{"osc"},
        &talas::project::ProjectNode::instanceId);
    if (auto error = require(
            localOscillator != localGraph.graph.nodes.end(),
            "workspace variable fixture could not find its oscillator"))
    {
        return error;
    }
    localOscillator->parameters.front().expression = "@localPitch";
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(std::move(localGraph)),
            "workspace variable expression authoring failed"))
    {
        return error;
    }
    documentChanged();
    const auto localFingerprint =
        talas::project::workspaceSessionVariableDeclarationFingerprint(
            localPitch);
    workspaceSession_.variableValues = {{
        "localPitch",
        talas::expr::ValueDomain::Hertz,
        120.0,
        localFingerprint,
    }};
    const talas::project::PreparedProjectVariableIdentity localPitchIdentity{
        talas::project::ProjectVariableOwnerKind::WorkspaceSession,
        "localPitch",
        {},
        {},
    };
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && applyWorkspaceSessionVariableValuesToCurrentStage(),
            "reconciled workspace value was not admitted to the exact prepared stage"))
    {
        return error;
    }
    currentDocumentStaged_ = true;
    if (auto error = require(
            renderVariableStage()
                && preparedValue(localPitchIdentity) == 120.0,
            "workspace value was not loaded after callback adoption"))
    {
        return error;
    }
    const auto localGeneration = projectBridge_.latestStageGeneration();
    inspectorPanel_->showVariables();
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            workspaceSession_.variableValues.size() == 1
                && workspaceSession_.variableValues.front().value == 120.0
                && inspectorPanel_->selectVariableForSmoke(
                    localPitch.owner, localPitch.variableId)
                && inspectorPanel_->selectPreparedVariableForSmoke(
                    localPitchIdentity, 0)
                && inspectorPanel_->preparedVariableReadyForSmoke(
                    "Workspace Session",
                    "local persistence after callback acknowledgment",
                    true,
                    false)
                && inspectorPanel_->assignSelectedVariableForSmoke(90.0)
                && workspaceSession_.variableValues.front().value == 120.0,
            "Workspace Session UI assignment should supersede an acknowledged restore without persisting before its own callback acknowledgment"))
    {
        return error;
    }
    if (auto error = require(
            renderVariableStage()
                && preparedValue(localPitchIdentity) == 90.0,
            "workspace assignment did not reach the exact callback"))
    {
        return error;
    }
    synchronizeWorkspaceSessionVariableValues();
    if (auto error = require(
            workspaceSession_.variableValues.front().value == 90.0
                && !projectBridge_.scheduleVariableAssignment(
                    localPitchIdentity,
                    60.0,
                    talas::expr::ValueDomain::Hertz,
                    0,
                    localGeneration - 1U)
                && workspaceSession_.variableValues.front().value == 90.0,
            "only callback-acknowledged exact-stage workspace assignments should persist"))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.scheduleVariableAssignment(
                localPitchIdentity,
                75.0,
                talas::expr::ValueDomain::Hertz,
                0,
                localGeneration)
                && renderVariableStage()
                && workspaceSession_.variableValues.front().value == 90.0
                && projectBridge_.compileAndStage(variableSettings).ok()
                && renderVariableStage()
                && preparedValue(localPitchIdentity) == 75.0,
            "compatible restaging should retain the newest live workspace value without replaying stale persisted state"))
    {
        return error;
    }
    synchronizeWorkspaceSessionVariableValues();
    if (auto error = require(
            workspaceSession_.variableValues.front().value == 75.0,
            "compatible-restage acknowledgment should update local persistence"))
    {
        return error;
    }
    inspectorPanel_->showVariables();
    inspectorPanel_->refreshVariableValues();
    if (auto error = require(
            inspectorPanel_->selectVariableForSmoke(
                localPitch.owner, localPitch.variableId)
                && inspectorPanel_->selectPreparedVariableForSmoke(
                    localPitchIdentity, 0)
                && inspectorPanel_->resetSelectedVariableForSmoke()
                && renderVariableStage(),
            "Workspace Session Inspector Reset did not reach the exact callback"))
    {
        return error;
    }
    synchronizeWorkspaceSessionVariableValues();
    if (auto error = require(
            workspaceSession_.variableValues.front().value == 240.0,
            "workspace variable Reset should persist only its acknowledged declared initial value"))
    {
        return error;
    }

    newProjectNow();
    if (auto error = require(
            workspaceSession_.variableValues.empty(),
            "document replacement should flush prior workspace variable authority"))
    {
        return error;
    }
    inspectorPanel_->focusExpressionLanguage();

    inspectorPanel_->commitExpressionDefinitionForSmoke(
        "half",
        "value:scalar",
        "value * 0.5");
    if (auto error = require(
            projectBridge_.editor().document()
                    .expressionDefinitions.definitions.size()
                    == 1
                && inspectorPanel_->expressionDefinitionReadyForSmoke(
                    "half", "value * 0.5", true)
                && projectBridge_.editor().canUndo()
                && !currentDocumentStaged_,
            "Variables v1 definition editor did not commit one stable, undoable definition"))
    {
        return error;
    }
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document()
                .expressionDefinitions.definitions.empty(),
            "definition editor Undo did not restore the empty library"))
    {
        return error;
    }
    redoProject();
    if (auto error = require(
            inspectorPanel_->expressionDefinitionReadyForSmoke(
                "half", "value * 0.5", true),
            "definition editor Redo did not restore exact field identity"))
    {
        return error;
    }
    inspectorPanel_->commitExpressionDefinitionForSmoke(
        "quarter",
        "value:scalar",
        "value * 0.25");
    if (auto error = require(
            projectBridge_.editor().document()
                    .expressionDefinitions.definitions.size()
                    == 2
                && inspectorPanel_->expressionDefinitionReadyForSmoke(
                    "quarter", "value * 0.25", true),
            "definition editor did not expose the newly selected neighbor fixture"))
    {
        return error;
    }
    inspectorPanel_->deleteExpressionDefinitionForSmoke();
    if (auto error = require(
            projectBridge_.editor().document()
                    .expressionDefinitions.definitions.size()
                    == 1
                && inspectorPanel_->expressionDefinitionReadyForSmoke(
                    "half", "value * 0.5", true),
            "definition deletion did not restore the nearest surviving definition identity"))
    {
        return error;
    }
    if (auto error = edit(
            projectBridge_.editor().addNode({
                "definitionCaller",
                "talas.constant",
                {expressionValue("half(2)")},
            }),
            "definition caller setup failed"))
    {
        return error;
    }
    documentChanged();
    inspectorPanel_->deleteExpressionDefinitionForSmoke();
    if (auto error = require(
            projectBridge_.editor().document()
                    .expressionDefinitions.definitions.size()
                    == 1
                && statusLabel_.getText().contains("delete blocked"),
            "definition editor did not expose dependency-in-use deletion truth"))
    {
        return error;
    }
    inspectorPanel_->commitExpressionForSmoke(
        "definitionCaller", "value", "2");
    inspectorPanel_->deleteExpressionDefinitionForSmoke();
    if (auto error = require(
            projectBridge_.editor().document()
                .expressionDefinitions.definitions.empty(),
            "definition editor did not delete an unused definition"))
    {
        return error;
    }

    auto legacyExpressionProject = talas::project::ProjectDocument{};
    legacyExpressionProject.formatVersion = 22;
    legacyExpressionProject.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    legacyExpressionProject.graph.nodes.push_back({
        "legacyConstant",
        "talas.constant",
        {expressionValue(
            std::string(
                talas::expr::ExpressionLimits::maximumSourceBytes,
                ' ')
                + "1")},
    });
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                legacyExpressionProject),
            "legacy expression smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    const auto rejectedAuthority =
        projectBridge_.editor().documentAuthorityGeneration();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Legacy expression", true, false, "bytes"),
            "incompatible legacy expression upgrade was not visibly unavailable with exact range evidence"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToSessionV1();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 22
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::LegacyV1
                && !projectBridge_.editor().canUndo()
                && projectBridge_.editor().documentAuthorityGeneration()
                    == rejectedAuthority
                && statusLabel_.getText().contains("bytes")
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Legacy expression", true, false, "bytes"),
            "failed expression upgrade changed authority/history or hid exact range evidence"))
    {
        return error;
    }

    legacyExpressionProject.graph.nodes.front()
        .parameters.front().expression = "1+2";
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                legacyExpressionProject),
            "compatible legacy expression smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Legacy expression", true, true),
            "compatible legacy expression upgrade was not enabled by the dry-run audit"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToSessionV1();
    inspectorPanel_->commitExpressionForSmoke(
        "legacyConstant", "value", "series(0, 1, 0)[0]");
    const auto wasContextHelpVisible =
        workspaceSession_.accessibility.contextHelpVisible;
    workspaceSession_.accessibility.contextHelpVisible = true;
    applyAccessibilityPreferences();
    inspectorPanel_->focusExpressionLanguage();
    auto* expressionLanguageFocus =
        inspectorPanel_->expressionLanguageComponentForSmoke();
    refreshContextHelp(expressionLanguageFocus);
    const auto expressionFocus =
        workbenchFocusIdentityForComponent(expressionLanguageFocus);
    const auto upgradedUiReady =
        projectBridge_.editor().document().formatVersion == 26
        && projectBridge_.editor().document().expressionLanguage
            == talas::expr::ExpressionLanguageVersion::SessionV1
        && projectBridge_.editor().canUndo()
        && !currentDocumentStaged_
        && statusLabel_.getText().contains("count")
        && projectBridge_.editor().document().graph.nodes.front()
                .parameters.front().expression
            == std::optional<std::string>{"1+2"}
        && inspectorPanel_->expressionLanguageReadyForSmoke(
            "Session state v1", true, true)
        && expressionFocus.has_value()
        && expressionFocus->itemId
            == "inspector.expression-language"
        && contextHelpPanel_->contentContainsForSmoke(
            "inspector.expression-language",
            "Session state v1");
    workspaceSession_.accessibility.contextHelpVisible =
        wasContextHelpVisible;
    applyAccessibilityPreferences();
    if (auto error = require(
            upgradedUiReady,
            "Session expression upgrade, invalid-index range, focus, or Context Help authority was not coherent"))
    {
        return error;
    }
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 22
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::LegacyV1
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Legacy expression", true, true),
            "Undo did not restore exact legacy expression authority"))
    {
        return error;
    }
    redoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 26
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::SessionV1
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Session state v1", true, true),
            "Redo did not restore exact Session state v1 authority"))
    {
        return error;
    }

    performProjectExpressionsUpgradeToVariablesV1();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 34
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV1
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v1", true, true),
            "SessionV1 project did not expose exact VariablesV1 predecessor authority"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToVariablesV2();
    inspectorPanel_->commitExpressionForSmoke(
        "legacyConstant", "value", "5 rem: 2");
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == talas::project::currentProjectFormatVersion
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV2
                && projectBridge_.editor().document().graph.nodes.front()
                       .parameters.front().expression
                    == std::optional<std::string>{"5 rem: 2"}
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v2", true, true),
            "VariablesV2 upgrade did not author exact rem: syntax or expose the voice upgrade action"))
    {
        return error;
    }
    undoProject();
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 34
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV1
                && projectBridge_.editor().document().graph.nodes.front()
                       .parameters.front().expression
                    == std::optional<std::string>{"1+2"}
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v1", true, true),
            "VariablesV2 Undo did not restore exact VariablesV1 bytes and action state"))
    {
        return error;
    }
    redoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == talas::project::currentProjectFormatVersion
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV2
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v2", true, true),
            "VariablesV2 Redo did not restore exact profile authority"))
    {
        return error;
    }

    const auto beforeVoiceUpgrade = currentDocumentSnapshot();
    performProjectExpressionsUpgradeToVoiceV1();
    if (auto error = require(
            projectBridge_.editor().document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VoiceV1
                && inspectorPanel_->expressionLanguageReadyForSmoke("Voice v1", false, false),
            "VoiceV1 upgrade did not expose the exact latest profile"))
        return error;
    const auto voiceUpgradeSnapshot = currentDocumentSnapshot();
    undoProject();
    if (auto error = require(currentDocumentSnapshot() == beforeVoiceUpgrade
            && inspectorPanel_->expressionLanguageReadyForSmoke("Variables v2", true, true),
            "VoiceV1 Undo did not restore exact VariablesV2 bytes and upgrade action"))
        return error;
    redoProject();
    if (auto error = require(currentDocumentSnapshot() == voiceUpgradeSnapshot
            && inspectorPanel_->expressionLanguageReadyForSmoke("Voice v1", false, false),
            "VoiceV1 Redo did not restore exact profile bytes and action state"))
        return error;

    talas::project::ProjectDocument timedProject;
    timedProject.graph.nodes = {
        {"timedOsc", "talas.oscillator",
            {expressionParameterForSmoke("frequency", "500 hz"),
             expressionParameterForSmoke("amplitude", "1")}},
        {"timedOut", "talas.audioOutput", {}},
    };
    timedProject.graph.connections = {
        {"timedOsc", "audioOut", "timedOut", "audioIn"}};
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(timedProject),
            "VariablesV3 Workbench smoke setup failed"))
        return error;
    documentChanged();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Variables v2", true, true)
                && inspectorPanel_->timedExpressionUpgradeReadyForSmoke(
                    true, true),
            "VariablesV2 Inspector did not show both Voice v1 and Variables v3 upgrades"))
        return error;
    performProjectExpressionsUpgradeToVariablesV3();
    inspectorPanel_->commitExpressionForSmoke(
        "timedOsc", "amplitude", "1 ramp: 4 ms");
    const auto timedCanonical = currentDocumentSnapshot();
    const auto timedReopened = talas::project::parseProjectDocument(
        timedCanonical);
    if (auto error = require(
            projectBridge_.editor().document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV3
                && projectBridge_.editor().document().graph.nodes.front()
                       .parameters[1].expression
                    == std::optional<std::string>{"1 ramp: 4 ms"}
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v3", true, true)
                && inspectorPanel_->timedExpressionUpgradeReadyForSmoke(
                    false, false)
                && timedReopened.ok()
                && timedReopened.document.expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV3,
            "Workbench did not author and reopen the timed expression under VariablesV3: "
                + statusLabel_.getText().toStdString()
                + " | authored="
                + projectBridge_.editor().document().graph.nodes.front()
                      .parameters[1].expression.value_or("<missing>")
                + " | reopened=" + (timedReopened.ok() ? "yes" : "no")))
        return error;
    undoProject();
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2
                && inspectorPanel_->timedExpressionUpgradeReadyForSmoke(
                    true, true),
            "VariablesV3 Undo did not restore the authoring upgrade action"))
        return error;
    redoProject();
    redoProject();
    if (auto error = require(currentDocumentSnapshot() == timedCanonical
            && inspectorPanel_->expressionLanguageReadyForSmoke(
                "Variables v3", true, true),
            "VariablesV3 Redo did not restore the exact timed project"))
        return error;

    performProjectExpressionsUpgradeToVariablesV4();
    const auto unitUpgradeCanonical = currentDocumentSnapshot();
    if (auto error = require(
            projectBridge_.editor().document().expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV4
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Variables v4", true, true)
                && inspectorPanel_->timedExpressionUpgradeReadyForSmoke(false, false),
            "VariablesV4 upgrade did not expose the postfix-unit profile"))
        return error;
    inspectorPanel_->commitExpressionForSmoke(
        "timedOsc", "frequency", "(60 + 9) nn");
    const auto unitCanonical = currentDocumentSnapshot();
    const auto unitReopened = talas::project::parseProjectDocument(unitCanonical);
    if (auto error = require(unitReopened.ok()
            && unitReopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV4
            && unitReopened.document.graph.nodes.front().parameters[0].expression
                == std::optional<std::string>{"(60 + 9) nn"}
            && unitReopened.document.graph.nodes.front().parameters[1].expression
                == std::optional<std::string>{"1 ramp: 4 ms"},
            "Workbench did not author and reopen postfix units with inherited timed expressions"))
        return error;
    undoProject();
    if (auto error = require(currentDocumentSnapshot() == unitUpgradeCanonical,
            "Undo did not restore the exact VariablesV4 upgrade before the unit edit"))
        return error;
    undoProject();
    if (auto error = require(currentDocumentSnapshot() == timedCanonical
            && inspectorPanel_->expressionLanguageReadyForSmoke("Variables v3", true, true),
            "VariablesV4 Undo did not restore the exact timed profile and upgrade action"))
        return error;
    redoProject();
    redoProject();
    if (auto error = require(currentDocumentSnapshot() == unitCanonical
            && inspectorPanel_->expressionLanguageReadyForSmoke("Variables v4", true, true),
            "VariablesV4 Redo did not restore exact postfix expression authority"))
        return error;

    performProjectExpressionsUpgradeToValV1();
    if (auto error = require(
            projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::ValV1
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Val v1", false, false),
            "Val v1 upgrade did not select Val's field syntax"))
        return error;
    undoProject();
    if (auto error = require(currentDocumentSnapshot() == unitCanonical
            && inspectorPanel_->expressionLanguageReadyForSmoke("Variables v4", true, true),
            "Val v1 Undo did not restore the exact VariablesV4 project"))
        return error;

    auto pureV2Project = talas::project::ProjectDocument{};
    pureV2Project.formatVersion = 23;
    pureV2Project.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV2;
    pureV2Project.graph.nodes.push_back({
        "pureV2Constant",
        "talas.constant",
        {expressionValue("[1, 2][1]")},
    });
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(pureV2Project),
            "PureV2 expression smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Pure expression v2", true, true),
            "PureV2 project did not expose an enabled SessionV1 upgrade"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToSessionV1();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 26
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::SessionV1
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Session state v1", true, true),
            "PureV2 project did not upgrade to exact format-26 SessionV1 authority"))
    {
        return error;
    }
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 23
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::PureV2
                && inspectorPanel_->expressionLanguageReadyForSmoke(
                    "Pure expression v2", true, true),
            "Undo did not restore exact format-23 PureV2 authority"))
    {
        return error;
    }
    auto pureV3Project = talas::project::ProjectDocument{};
    pureV3Project.formatVersion = 24;
    pureV3Project.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV3;
    pureV3Project.graph.nodes.push_back({
        "pureV3Constant",
        "talas.constant",
        {expressionValue("series(1, 1, 2)[1]")},
    });
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(pureV3Project),
            "PureV3 expression smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Pure expression v3", true, true),
            "PureV3 project did not expose an enabled SessionV1 upgrade"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToSessionV1();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 26
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::SessionV1
                && projectBridge_.editor().document()
                       .expressionDefinitions.definitions.empty(),
            "PureV3 project did not upgrade to empty-library format-26 SessionV1 authority"))
    {
        return error;
    }

    auto pureV4Project = talas::project::ProjectDocument{};
    pureV4Project.formatVersion = 25;
    pureV4Project.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV4;
    pureV4Project.expressionDefinitions.definitions.push_back({
        "half",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value * 0.5",
    });
    pureV4Project.graph.nodes.push_back({
        "pureV4Constant",
        "talas.constant",
        {expressionValue("half(2)")},
    });
    if (auto error = edit(
            projectBridge_.editor().replaceDocument(pureV4Project),
            "PureV4 expression smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    if (auto error = require(
            inspectorPanel_->expressionLanguageReadyForSmoke(
                "Pure expression v4", true, true),
            "PureV4 project did not expose an enabled SessionV1 upgrade"))
    {
        return error;
    }
    performProjectExpressionsUpgradeToSessionV1();
    currentDocumentStaged_ = true;
    inspectorPanel_->commitExpressionForSmoke(
        "pureV4Constant", "value", "previous(!Input, 0)");
    const auto wasStateContextHelpVisible =
        workspaceSession_.accessibility.contextHelpVisible;
    workspaceSession_.accessibility.contextHelpVisible = true;
    applyAccessibilityPreferences();
    auto* expressionValueFocus =
        inspectorPanel_->expressionValueComponentForSmoke();
    refreshContextHelp(expressionValueFocus);
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 26
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::SessionV1
                && projectBridge_.editor().document()
                       .expressionDefinitions.definitions
                    == pureV4Project.expressionDefinitions.definitions
                && projectBridge_.editor().document().graph.nodes.front()
                       .parameters.front().expression
                    == std::optional<std::string>{"previous(!Input, 0)"}
                && inspectorPanel_->parameterHintContainsForSmoke(
                    "State slots: 1/32")
                && inspectorPanel_->parameterHintContainsForSmoke(
                    "one accepted live Event Value update")
                && inspectorPanel_->parameterHintContainsForSmoke(
                    "not project or snapshot state")
                && contextHelpPanel_->contentContainsForSmoke(
                    "inspector.parameter.value",
                    "State slots: 1/32")
                && !currentDocumentStaged_,
            "PureV4 upgrade did not retain definitions, author one bounded state slot, or invalidate the prepared stage"))
    {
        workspaceSession_.accessibility.contextHelpVisible =
            wasStateContextHelpVisible;
        applyAccessibilityPreferences();
        return error;
    }
    workspaceSession_.accessibility.contextHelpVisible =
        wasStateContextHelpVisible;
    applyAccessibilityPreferences();
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().graph.nodes.front()
                    .parameters.front().expression
                == std::optional<std::string>{"half(2)"}
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::SessionV1,
            "Undo did not restore the exact pre-stateful expression while retaining SessionV1 authority"))
    {
        return error;
    }
    undoProject();
    if (auto error = require(
            projectBridge_.editor().document().formatVersion == 25
                && projectBridge_.editor().document().expressionLanguage
                    == talas::expr::ExpressionLanguageVersion::PureV4
                && projectBridge_.editor().document()
                       .expressionDefinitions.definitions
                    == pureV4Project.expressionDefinitions.definitions,
            "Second Undo did not restore exact format-25 PureV4 bytes and definitions"))
    {
        return error;
    }

    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                midiMapperDocumentForSmoke("velocity")),
            "MIDI Mapper Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    projectBridge_.setPlaying(true);
    const auto renderMapperStage = [&](double callbackTimeSeconds) {
        return projectBridge_.render(
            {},
            callbackTimeSeconds,
            variableChannels.data(),
            variableChannels.size(),
            variableOutput.size());
    };
    const auto mapperStage =
        projectBridge_.compileAndStage(variableSettings);
    if (auto error = require(
            mapperStage.ok(),
            "mapped Workbench stage did not compile: "
                + mapperStage.message))
    {
        return error;
    }
    if (auto error = require(
            renderMapperStage(0.0),
            "mapped Workbench baseline callback did not render"))
    {
        return error;
    }
    if (auto error = require(
            std::ranges::all_of(variableOutput, [](float sample) {
                return std::abs(sample) <= 1.0e-6F;
            }),
            "mapped Workbench stage did not begin with a closed gate"))
    {
        return error;
    }
    selectNode("mapper");
    refreshPreparedGraphStatus();
    const auto mapperPreparedStatus = projectBridge_.preparedGraphStatus();
    const auto mapperPreparedContract = preparedInspectorContract(
        projectBridge_.editor().document(),
        std::nullopt,
        "mapper",
        mapperPreparedStatus);
    const auto expectedVelocityMapperContract = juceString(
        "MIDI Mapper · mode velocity · ready · channel any · pitch 0...127 · polyphony 4");
    if (auto error = require(
            mapperPreparedStatus.prepared != nullptr
                && mapperPreparedStatus.prepared->midiMappers.size() == 1
                && mapperPreparedContract.contains(
                    expectedVelocityMapperContract)
                && inspectorPanel_->preparedContractContainsForSmoke(
                    expectedVelocityMapperContract),
            "Inspector did not expose exact compiled MIDI Mapper mode authority: "
                + mapperPreparedContract.toStdString()))
    {
        return error;
    }
    if (auto error = require(
            projectBridge_.enqueueMidiNoteOn(1, 60, 64, 1.0)
                && renderMapperStage(2.0),
            "mapped Workbench velocity-64 note did not render"))
    {
        return error;
    }
    const auto velocity64 = variableOutput;
    if (auto error = require(
            std::ranges::any_of(velocity64, [](float sample) {
                return std::abs(sample) > 1.0e-4F;
            })
                && projectBridge_.enqueueMidiNoteOn(2, 61, 127, 3.0)
                && renderMapperStage(4.0),
            "mapped Workbench maximum-held velocity did not render"))
    {
        return error;
    }
    const auto velocity127 = variableOutput;
    const auto mappedTransitionFrame = velocity64.size() / 2U;
    for (std::size_t frame = 0; frame < velocity64.size(); ++frame)
    {
        const auto expected = frame < mappedTransitionFrame
            ? velocity64[frame]
            : velocity64[frame] * (127.0F / 64.0F);
        if (auto error = require(
                std::abs(velocity127[frame] - expected) <= 1.0e-5F,
                "mapped Workbench velocity gain was not exact at frame "
                    + std::to_string(frame)
                    + ": v64=" + std::to_string(velocity64[frame])
                    + " v127=" + std::to_string(velocity127[frame])))
        {
            return error;
        }
    }
    if (auto error = require(
            projectBridge_.enqueueMidiNoteOff(2, 61, 5.0)
                && renderMapperStage(6.0),
            "mapped Workbench maximum note-off did not render"))
    {
        return error;
    }
    for (std::size_t frame = 0; frame < velocity64.size(); ++frame)
    {
        const auto expected = frame < mappedTransitionFrame
            ? velocity64[frame] * (127.0F / 64.0F)
            : velocity64[frame];
        if (auto error = require(
                std::abs(variableOutput[frame] - expected) <= 1.0e-5F,
                "mapped Workbench note-off was not exact at frame "
                    + std::to_string(frame)
                    + ": actual=" + std::to_string(variableOutput[frame])
                    + " expected=" + std::to_string(expected)))
        {
            return error;
        }
    }
    if (auto error = require(
            projectBridge_.enqueueMidiNoteOff(1, 60, 7.0)
                && renderMapperStage(8.0),
            "mapped Workbench final note-off did not close the gate"))
    {
        return error;
    }
    for (std::size_t frame = 0; frame < velocity64.size(); ++frame)
    {
        const auto expected = frame < mappedTransitionFrame
            ? velocity64[frame]
            : 0.0F;
        if (auto error = require(
                std::abs(variableOutput[frame] - expected) <= 1.0e-5F,
                "mapped Workbench final note-off was not exact at frame "
                    + std::to_string(frame)))
        {
            return error;
        }
    }
    const auto staleRejectedBefore =
        projectBridge_.rejectedRealtimeControlCount();
    if (auto error = require(
            projectBridge_.enqueueMidiNoteOn(1, 62, 127, 9.0)
                && projectBridge_.compileAndStage(variableSettings).ok()
                && renderMapperStage(10.0)
                && projectBridge_.rejectedRealtimeControlCount()
                    == staleRejectedBefore + 1
                && std::ranges::all_of(variableOutput, [](float sample) {
                    return std::abs(sample) <= 1.0e-6F;
                }),
            "mapped Workbench replacement did not reject stale-stage MIDI and reset held state"))
    {
        return error;
    }

    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                midiMapperDocumentForSmoke("gate")),
            "MIDI Mapper gate Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    projectBridge_.setPlaying(true);
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok()
                && renderMapperStage(11.0)
                && std::ranges::all_of(variableOutput, [](float sample) {
                    return std::abs(sample) <= 1.0e-6F;
                })
                && projectBridge_.enqueueMidiNoteOn(1, 63, 1, 11.0)
                && renderMapperStage(12.0)
                && std::ranges::all_of(variableOutput, [](float sample) {
                    return std::abs(sample - 1.0F) <= 1.0e-6F;
                })
                && projectBridge_.enqueueMidiNoteOff(1, 63, 12.0)
                && renderMapperStage(13.0)
                && std::ranges::all_of(variableOutput, [](float sample) {
                    return std::abs(sample) <= 1.0e-6F;
                }),
            "mapped Workbench gate mode did not open and close through product MIDI ingress"))
    {
        return error;
    }

    if (auto error = edit(
            projectBridge_.editor().replaceDocument(
                midiMapperDocumentForSmoke("")),
            "blank MIDI Mapper Workbench smoke setup failed"))
    {
        return error;
    }
    documentChanged();
    projectBridge_.setPlaying(true);
    if (auto error = require(
            projectBridge_.compileAndStage(variableSettings).ok(),
            "blank MIDI Mapper Workbench stage did not compile"))
    {
        return error;
    }
    for (std::size_t callback = 0; callback < 64; ++callback)
    {
        if (auto error = require(
                renderMapperStage(
                    14.0 + static_cast<double>(callback) * 0.001),
                "blank MIDI Mapper transition did not remain renderable"))
        {
            return error;
        }
    }
    selectNode("mapper");
    refreshPreparedGraphStatus();
    if (auto error = require(
            std::ranges::all_of(variableOutput, [](float sample) {
                return std::abs(sample - 1.0F) <= 1.0e-6F;
                })
                && inspectorPanel_->preparedContractContainsForSmoke(
                    juceString(
                        "MIDI Mapper · mode blank passthrough · ready · no MIDI claimed")),
            "blank MIDI Mapper did not preserve exact passthrough and compiled Inspector truth"))
    {
        return error;
    }

    // Exercise Source editing and the same resource binding used after Browse.
    for (const bool mapper : {false, true})
    {
        const std::string voiceKind = mapper ? "MIDI Mapper" : "MIDI Voice";
        newProjectNow();
        talas::project::ProjectDocument fileVoiceDocument;
        fileVoiceDocument.graph.nodes = {
            {"osc", "talas.oscillator", {
                expressionParameterForSmoke("frequency", "7 hz"),
                expressionParameterForSmoke("amplitude", "(!KeyDown > 0) * !KeyVelocity"),
            }},
            midiVoiceForSmoke("fileVoice", 1),
            {"out", "talas.audioOutput", {}},
        };
        if (mapper)
        {
            auto& voice = fileVoiceDocument.graph.nodes[1];
            voice.typeId = "talas.midiMapper";
            voice.parameters.push_back(textParameterForSmoke("map", ""));
            voice.parameters.push_back(choiceParameterForSmoke(
                "voiceModel", "polyphonicInput"));
        }
        fileVoiceDocument.graph.connections = {
            {"osc", "audioOut", "fileVoice", "audioIn"},
            {"fileVoice", "leftOut", "out", "audioIn"},
            {"fileVoice", "rightOut", "out", "rightIn"},
        };
        if (auto error = edit(projectBridge_.editor().replaceDocument(fileVoiceDocument), voiceKind + " file setup"))
            return error;
        if (!projectBridge_.editor().moveNode("osc", {0, 0}).ok()
            || !projectBridge_.editor().moveNode("fileVoice", {240, 0}).ok()
            || !projectBridge_.editor().moveNode("out", {480, 0}).ok())
            return voiceKind + " fixture layout failed";
        documentChanged();
        selectNode("fileVoice");
        inspectorPanel_->commitExpressionForSmoke("fileVoice", "source", "MIDI File");
        const auto midiPath = smokeDirectory / (mapper
            ? "workbench-midi-mapper-voice.mid" : "workbench-midi-voice.mid");
        const std::array<std::uint8_t, 34> midiBytes = {
            'M','T','h','d',0,0,0,6,0,0,0,1,1,0xf4,
            'M','T','r','k',0,0,0,12,0,0x90,60,127,20,0x80,60,0,0,0xff,0x2f,0,
        };
        {
            std::ofstream midiOutput(midiPath, std::ios::binary);
            midiOutput.write(reinterpret_cast<const char*>(midiBytes.data()), midiBytes.size());
            if (!midiOutput.good()) return voiceKind + " smoke could not write its MIDI fixture";
        }
        bindResourceFile(std::nullopt, "fileVoice", "midiFile", midiPath);
        const auto fileReferences = [&] {
            return talas::audio::reachableProjectAssetReferences(projectBridge_.editor().document(), registry_);
        };
        if (auto error = require(projectBridge_.editor().document().assets.size() == 1
                && projectBridge_.editor().document().assets.front().kind == talas::project::ProjectAssetKind::Binary
                && fileReferences().ok() && fileReferences().references.size() == 1,
                "Workbench " + voiceKind + " file binding or Inspector Source commit failed"))
            return error;
        undoProject();
        if (!fileReferences().ok() || !fileReferences().references.empty()) return voiceKind + " Undo did not remove the MIDI resource binding";
        redoProject();
        if (fileReferences().references.size() != 1) return voiceKind + " Redo did not restore the MIDI resource binding";
        inspectorPanel_->commitExpressionForSmoke("fileVoice", "source", "Live MIDI");
        if (!fileReferences().ok() || !fileReferences().references.empty()) return voiceKind + " Inspector Live MIDI edit did not deactivate the file";
        undoProject();
        if (fileReferences().references.size() != 1) return voiceKind + " Source Undo did not reactivate the file";
        const auto fileProjectPath = smokeDirectory / (mapper
            ? "workbench-midi-mapper-voice.talas.json" : "workbench-midi-voice.talas.json");
        if (!saveProjectToPath(fileProjectPath, {})) return "Workbench " + voiceKind + " save failed";
        newProjectNow();
        loadProjectFromPath(fileProjectPath);
        if (!fileReferences().ok() || fileReferences().references.size() != 1)
            return "Workbench " + voiceKind + " reopen lost its selected resource";
        talas::runtime::RenderSettings fileSettings{1000.0, 32};
        fileSettings.eventValues = {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber},
            {"KeyDown", 0}, {"KeyVelocity", 0}};
        projectBridge_.stopTransport();
        if (!stageProject(fileSettings, voiceKind + " file smoke")) return "Workbench " + voiceKind + " resource staging failed";
        projectBridge_.setPlaying(true);
        std::array<float, 32> fileLeft{}, fileRight{};
        std::array<float*, 2> fileChannels{fileLeft.data(), fileRight.data()};
        // Finish the existing graph-host transition, then replay from a stopped
        // boundary so the independent sample oracle starts at transport frame zero.
        for (std::size_t block = 0; block < 32; ++block)
            if (!projectBridge_.render({}, 100.0 + block * 0.032, fileChannels.data(), 2, 32))
                return "Workbench " + voiceKind + " transition did not render";
        projectBridge_.stopTransport();
        projectBridge_.setPlaying(true);
        if (!projectBridge_.render({}, 102.0, fileChannels.data(), 2, 32))
            return "Workbench " + voiceKind + " replay did not render";
        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            const auto expected = frame >= 5 && frame < 20
                ? std::sin(2.0 * std::numbers::pi * 7.0 * frame / 1000.0) : 0.0;
            if (std::abs(fileLeft[frame] - expected) > 1.0e-5
                || std::abs(fileRight[frame] - expected) > 1.0e-5)
                return "Workbench " + voiceKind + " PCM differs at frame " + std::to_string(frame)
                    + ": " + std::to_string(fileLeft[frame]) + " expected " + std::to_string(expected);
        }
        projectBridge_.stopTransport();
        selectNode("fileVoice");
        refreshPreparedGraphStatus();
        if (mapper)
        {
            const auto status = projectBridge_.preparedGraphStatus();
            const auto expected = juceString(
                "MIDI Mapper · polyphonic Input · ready · channel 1 · pitch 0...127 · polyphony 1");
            if (auto error = require(
                    status.prepared != nullptr
                        && status.prepared->midiMappers.size() == 1
                        && status.prepared->midiMappers.front().polyphonicInput
                        && preparedInspectorContract(
                            projectBridge_.editor().document(),
                            std::nullopt, "fileVoice", status).contains(expected)
                        && inspectorPanel_->preparedContractContainsForSmoke(expected),
                    "Workbench MIDI Mapper Inspector must report prepared polyphonic Input authority"))
                return error;
        }
        inspectorPanel_->focusParameter("source");
        graphCanvas_->zoomToFit(false);
        const auto fileImage = createComponentSnapshot(getLocalBounds(), true, 1.0F);
        auto fileImageOutput = juce::File{juceString((smokeDirectory / (mapper
            ? "workbench-midi-mapper-voice.png" : "workbench-midi-voice.png")).string())}.createOutputStream();
        if (!fileImage.isValid() || !fileImageOutput || !fileImageOutput->openedOk()
            || !fileImageOutput->setPosition(0) || fileImageOutput->truncate().failed()
            || !juce::PNGImageFormat{}.writeImageToStream(fileImage, *fileImageOutput))
            return "Workbench " + voiceKind + " screenshot failed";
        fileImageOutput->flush();
    }

    // Exercise Script and live authoring on the same polyphonic Mapper while
    // an unavailable MIDI file remains an inactive project field.
    newProjectNow();
    talas::project::ProjectDocument scriptMapperDocument;
    scriptMapperDocument.graph.nodes = {
        {"osc", "talas.oscillator", {
            expressionParameterForSmoke("frequency", "7 hz"),
            expressionParameterForSmoke("amplitude", "(!KeyDown > 0) * !KeyVelocity"),
        }},
        midiVoiceForSmoke("scriptMapper", 1),
        {"out", "talas.audioOutput", {}},
    };
    auto& scriptMapperNode = scriptMapperDocument.graph.nodes[1];
    scriptMapperNode.typeId = "talas.midiMapper";
    scriptMapperNode.parameters.push_back(textParameterForSmoke("map", ""));
    scriptMapperNode.parameters.push_back(choiceParameterForSmoke(
        "voiceModel", "polyphonicInput"));
    const auto inactiveFile = std::ranges::find(
        scriptMapperNode.parameters, std::string_view{"midiFile"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (inactiveFile == scriptMapperNode.parameters.end())
        return "Workbench MIDI Mapper Script fixture lacks a MIDI file field";
    inactiveFile->resource = "unavailable-midi";
    scriptMapperDocument.assets = {{"unavailable-midi",
        talas::project::ProjectAssetKind::Binary, "unavailable.mid"}};
    scriptMapperDocument.graph.connections = {
        {"osc", "audioOut", "scriptMapper", "audioIn"},
        {"scriptMapper", "leftOut", "out", "audioIn"},
        {"scriptMapper", "rightOut", "out", "rightIn"},
    };
    if (auto error = edit(projectBridge_.editor().replaceDocument(
            scriptMapperDocument), "MIDI Mapper Script setup"))
        return error;
    documentChanged();
    selectNode("scriptMapper");
    const std::string literalScript =
        "self keyDownAt: 0 ms duration: 20 ms frequency: 445 hz velocity: 0.7.\n"
        "self keyDownAt: 100 ms duration: 10 ms frequency: 446 hz velocity: 0.5.";
    inspectorPanel_->commitExpressionForSmoke("scriptMapper", "source", "Script");
    inspectorPanel_->commitExpressionForSmoke("scriptMapper", "script", literalScript);
    const auto scriptReferences = [&] {
        return talas::audio::reachableProjectAssetReferences(
            projectBridge_.editor().document(), registry_);
    };
    if (!scriptReferences().ok() || !scriptReferences().references.empty())
        return "Workbench Mapper Script selected an inactive missing MIDI file";
    undoProject();
    redoProject();
    const auto scriptProjectPath = smokeDirectory / "workbench-midi-mapper-script.talas.json";
    if (!saveProjectToPath(scriptProjectPath, {}))
        return "Workbench Mapper Script save failed";
    newProjectNow();
    loadProjectFromPath(scriptProjectPath);
    const auto& savedMapper = projectBridge_.editor().document().graph.nodes[1];
    const auto savedScript = std::ranges::find(
        savedMapper.parameters, std::string_view{"script"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (savedScript == savedMapper.parameters.end()
        || savedScript->text != literalScript
        || !scriptReferences().ok()
        || !scriptReferences().references.empty())
        return "Workbench Mapper Script reopen lost the program or selected an inactive file";
    talas::runtime::RenderSettings scriptSettings{1000.0, 32};
    scriptSettings.eventValues = {{"Pitch", 60, talas::expr::ValueDomain::NoteNumber},
        {"KeyDown", 0}, {"KeyVelocity", 0}};
    projectBridge_.stopTransport();
    if (!stageProject(scriptSettings, "MIDI Mapper Script smoke"))
        return "Workbench Mapper Script staging failed";
    projectBridge_.setPlaying(true);
    std::array<float, 32> scriptLeft{}, scriptRight{};
    std::array<float*, 2> scriptChannels{scriptLeft.data(), scriptRight.data()};
    for (std::size_t block = 0; block < 32; ++block)
        if (!projectBridge_.render({}, 300.0 + block * 0.032,
                scriptChannels.data(), 2, 32))
            return "Workbench Mapper Script transition did not render";
    projectBridge_.stopTransport();
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render({}, 302.0, scriptChannels.data(), 2, 32))
        return "Workbench Mapper Script replay did not render";
    for (std::size_t frame = 0; frame < 32; ++frame)
    {
        const auto expected = frame >= 5 && frame < 20
            ? 0.7 * std::sin(2.0 * std::numbers::pi * 7.0 * frame / 1000.0)
            : 0.0;
        if (std::abs(scriptLeft[frame] - expected) > 1.0e-5
            || std::abs(scriptRight[frame] - expected) > 1.0e-5)
            return "Workbench Mapper Script PCM differs at frame "
                + std::to_string(frame);
    }
    projectBridge_.stopTransport();
    selectNode("scriptMapper");
    refreshPreparedGraphStatus();
    const auto scriptStatus = projectBridge_.preparedGraphStatus();
    const auto scriptContract = preparedInspectorContract(
        projectBridge_.editor().document(), std::nullopt,
        "scriptMapper", scriptStatus);
    const auto expectedScriptContract = juceString(
        "MIDI Mapper · polyphonic Input · ready");
    inspectorPanel_->focusParameter("script");
    const auto* visibleScript = static_cast<juce::TextEditor*>(
        inspectorPanel_->expressionValueComponentForSmoke());
    if (visibleScript == nullptr
        || !visibleScript->isMultiLine()
        || !visibleScript->getReturnKeyStartsNewLine()
        || visibleScript->getBounds().getHeight() < 72
        || visibleScript->getText().toStdString() != literalScript)
        return "Workbench Mapper Script Inspector must show two editable lines and the authored program";
    const auto scriptImage = createComponentSnapshot(getLocalBounds(), true, 1.0F);
    auto scriptImageOutput = juce::File{juceString((smokeDirectory
        / "workbench-midi-mapper-script.png").string())}.createOutputStream();
    if (!scriptImage.isValid() || !scriptImageOutput
        || !scriptImageOutput->openedOk()
        || !scriptImageOutput->setPosition(0)
        || scriptImageOutput->truncate().failed()
        || !juce::PNGImageFormat{}.writeImageToStream(
            scriptImage, *scriptImageOutput))
        return "Workbench Mapper Script screenshot failed";
    scriptImageOutput->flush();
    if (scriptStatus.prepared == nullptr
        || scriptStatus.prepared->midiMappers.size() != 1
        || !scriptStatus.prepared->midiMappers.front().polyphonicInput
        || !scriptContract.contains(expectedScriptContract)
        || !inspectorPanel_->preparedContractContainsForSmoke(
            expectedScriptContract))
        return "Workbench Mapper Script Inspector lost polyphonic authority: "
            + scriptContract.toStdString();

    inspectorPanel_->commitExpressionForSmoke("scriptMapper", "script", "");
    inspectorPanel_->commitExpressionForSmoke("scriptMapper", "source", "Live MIDI");
    if (!scriptReferences().ok() || !scriptReferences().references.empty())
        return "Workbench Mapper live source selected an inactive missing MIDI file";
    const auto liveProjectPath = smokeDirectory / "workbench-midi-mapper-live.talas.json";
    if (!saveProjectToPath(liveProjectPath, {}))
        return "Workbench Mapper live source save failed";
    newProjectNow();
    loadProjectFromPath(liveProjectPath);
    if (!scriptReferences().ok() || !scriptReferences().references.empty())
        return "Workbench Mapper live source reopen selected an inactive file";
    if (!stageProject(scriptSettings, "MIDI Mapper live smoke"))
        return "Workbench Mapper live source staging failed";
    projectBridge_.setPlaying(true);
    std::array<float, 32> liveLeft{}, liveRight{};
    std::array<float*, 2> liveChannels{liveLeft.data(), liveRight.data()};
    for (std::size_t block = 0; block < 32; ++block)
        if (!projectBridge_.render({}, 400.0 + block * 0.032,
                liveChannels.data(), 2, 32))
            return "Workbench Mapper live transition did not render";
    projectBridge_.stopTransport();
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render({}, 402.0, liveChannels.data(), 2, 32)
        || !std::ranges::all_of(liveLeft, [](float sample) {
            return std::abs(sample) <= 1.0e-6F;
        }))
        return "Workbench Mapper live source must begin silent before a note";
    if (!projectBridge_.enqueueMidiNoteOn(1, 60, 127, 402.01)
        || !projectBridge_.render({}, 402.032, liveChannels.data(), 2, 32)
        || !std::ranges::any_of(liveLeft, [](float sample) {
            return std::abs(sample) > 1.0e-4F;
        }))
        return "Workbench Mapper live source did not trigger an Input voice";
    projectBridge_.stopTransport();

    newProjectNow();
    talas::project::ProjectVariableDeclaration variablePreview;
    variablePreview.variableId = "gain";
    variablePreview.displayName = "Project gain";
    variablePreview.initialValue = 0.25;
    inspectorPanel_->createVariableForSmoke(variablePreview);
    inspectorPanel_->showVariables();
    resized();
    if (auto error = require(
            inspectorPanel_->variableNumbersAreStrictForSmoke(),
            "Variables UI accepted a malformed numeric token as zero"))
    {
        return error;
    }
    const auto image = createComponentSnapshot(getLocalBounds(), true, 1.0F);
    if (!image.isValid())
        return "Workbench Variables snapshot did not produce a valid image";
    auto stream = juce::File{juceString(
        (smokeDirectory / "workbench-variables.png").lexically_normal().string())}
                      .createOutputStream();
    if (stream == nullptr || !stream->openedOk())
        return "Workbench Variables snapshot output could not be opened";
    if (!stream->setPosition(0) || stream->truncate().failed())
        return "Workbench Variables snapshot output could not be replaced";
    if (!juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        return "Workbench Variables snapshot PNG encoding failed";
    stream->flush();
    undoProject();
    inspectorPanel_->focusExpressionLanguage();
    return {};
}

std::optional<std::string> WorkbenchComponent::runMidiMapperEditorSmoke(
    const std::filesystem::path& smokeDirectory,
    const std::string& returnSelectionId)
{
    addPrototypeAt("talas.midiMapper", {140, 420});
    const auto& nodes = projectBridge_.editor().document().graph.nodes;
    const auto mapper = std::ranges::find_if(nodes, [](const auto& node) {
        return node.typeId == "talas.midiMapper";
    });
    if (mapper == nodes.end())
        return "MIDI Mapper Map editor smoke node was not created";
    const auto mapperId = mapper->instanceId;
    const std::string mapLines =
        "!SmokeA is: `SmokeB\n!SmokeC is: `SmokeD";
    inspectorPanel_->commitExpressionForSmoke(mapperId, "map", mapLines);
    const auto* mapEditor = static_cast<juce::TextEditor*>(
        inspectorPanel_->expressionValueComponentForSmoke());
    const auto* mapperNode = findNode(
        projectBridge_.editor().document(), mapperId);
    if (mapEditor == nullptr || !mapEditor->isMultiLine()
        || !mapEditor->getReturnKeyStartsNewLine()
        || mapEditor->getBounds().getHeight() < 72
        || mapEditor->getText().toStdString() != mapLines
        || !inspectorPanel_->parameterHintContainsForSmoke(
            "independent Input copies")
        || !inspectorPanel_->parameterHintContainsForSmoke(
            "gate/velocity tokens require legacyPassthrough")
        || mapperNode == nullptr
        || std::ranges::none_of(mapperNode->parameters,
            [&](const auto& parameter) {
                return parameter.parameterId == "map"
                    && parameter.text == mapLines;
            }))
    {
        return "MIDI Mapper Map editor must retain two visible lines, its project value and the voice model guidance";
    }

    const auto image = createComponentSnapshot(
        getLocalBounds(), true, 1.0F);
    if (!image.isValid())
        return "Workbench snapshot did not produce a valid image";
    const auto path = (smokeDirectory
        / "workbench-midi-mapper-map-editor.png").lexically_normal();
    auto stream = juce::File{juceString(path.string())}.createOutputStream();
    if (stream == nullptr || !stream->openedOk())
        return "Workbench snapshot output could not be opened";
    if (!stream->setPosition(0) || stream->truncate().failed())
        return "Workbench snapshot output could not be replaced";
    if (!juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        return "Workbench snapshot PNG encoding failed";
    stream->flush();

    const auto removed = projectBridge_.editor().removeNode(mapperId);
    if (!removed.ok())
    {
        return "temporary MIDI Mapper Map editor node removal failed: "
            + removed.message;
    }
    documentChanged();
    selectNode(returnSelectionId);
    return {};
}

std::optional<std::string>
WorkbenchComponent::runMidiMapperVirtualOnlyControlSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{1000.0, 64};
            if (!stageProject(
                    std::move(restoreSettings),
                    "virtual-only MIDI Mapper smoke restore")
                && error.empty())
            {
                error = "virtual-only MIDI Mapper smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "virtual-only MIDI Mapper smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };

    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };
    const auto text = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto choice = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };

    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition input;
    input.definitionId = "virtualOnlyMapInput";
    input.displayName = "Virtual Only Map Input";
    input.graph.nodes = {{"osc", "talas.oscillator", {
        expression("frequency", "100 hz"),
        expression("amplitude", "!LocalLevel * (!KeyDown > 0)"),
    }}};
    input.outputs = {{
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(input));
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mapper", "talas.midiMapper", {
            text("map", "!LocalLevel is: `LocalLevel"),
            choice("voiceModel", "polyphonicInput"),
            expression("channel", "2"),
            expression("polyphony", "1"),
            expression("lowPitch", "0 nn"),
            expression("highPitch", "127 nn"),
        }},
        {"source", "", {}, "virtualOnlyMapInput"},
    };
    document.graph.connections = {
        {"source", "audioOut", "mapper", "audioIn"},
        {"mapper", "leftOut", "out", "audioIn"},
        {"mapper", "rightOut", "out", "rightIn"},
    };

    const auto replaced = projectBridge_.editor().replaceDocument(document);
    if (!replaced.ok())
    {
        return finish(
            "virtual-only MIDI Mapper project was rejected: "
            + replaced.message);
    }
    documentChanged();
    const auto& controls = projectBridge_.editor().document().virtualControls;
    const auto control = std::ranges::find(
        controls, "LocalLevel", &talas::project::VirtualControl::eventName);
    if (control == controls.end()
        || control->kind != talas::project::VirtualControlKind::Fader
        || control->midiMapping.has_value()
        || !virtualControlSurface_->focusEventValueForSmoke("LocalLevel")
        || virtualControlSurface_->focusedEventValueForSmoke()
            != std::optional<std::string>{"LocalLevel"})
    {
        return finish(
            "same-name local Map did not expose an unmapped, focusable fader");
    }

    talas::runtime::RenderSettings settings{1000.0, 64};
    settings.defaultMidiChannel = 2;
    if (!stageProject(
            std::move(settings), "virtual-only MIDI Mapper smoke"))
    {
        return finish(
            "virtual-only MIDI Mapper Workbench stage failed: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    const auto silent = [&] {
        return std::ranges::all_of(left, [](float sample) {
            return std::abs(sample) < 1.0e-6F;
        });
    };
    if (!projectBridge_.render(
            {}, 10.0, channels.data(), channels.size(), left.size())
        || !silent()
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 10.0)
        || !projectBridge_.render(
            {}, 10.064, channels.data(), channels.size(), left.size())
        || !silent())
    {
        return finish(
            "same-name local Map must stay silent until its fader is moved");
    }
    if (!virtualControlSurface_->setValueForSmoke("LocalLevel", 0.5)
        || !projectBridge_.render(
            {}, 10.128, channels.data(), channels.size(), left.size())
        || std::ranges::none_of(left, [](float sample) {
            return std::abs(sample) > 0.2F;
        }))
    {
        return finish(
            "Workbench fader did not drive the triggered local Input");
    }
    if (projectBridge_.enqueueMidiControlChange(2, 18, 127, 10.192)
        || !virtualControlSurface_->setValueForSmoke("LocalLevel", 0.0)
        || !projectBridge_.render(
            {}, 10.192, channels.data(), channels.size(), left.size())
        || !silent())
    {
        return finish(
            "unmapped physical CC changed the virtual-only local Input");
    }
    return finish();
}

std::optional<std::string>
WorkbenchComponent::runMidiMapperRangedLocalControlSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{1000.0, 64};
            if (!stageProject(std::move(restoreSettings),
                    "ranged local Map smoke restore")
                && error.empty())
            {
                error = "ranged local Map smoke could not restore the prior stage";
            }
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "ranged local Map smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };
    const auto map = [](std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = "map";
        parameter.text = std::move(value);
        return parameter;
    };
    const auto choice = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    const auto shared = [] {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = "shared";
        parameter.boolean = true;
        return parameter;
    };

    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition input;
    input.definitionId = "rangedMapInput";
    input.displayName = "Ranged Map Input";
    input.graph.nodes = {{"osc", "talas.oscillator", {
        expression("frequency", "100 hz"),
        expression("amplitude", "!LocalLevel * (!KeyDown > 0)"),
    }}};
    input.outputs = {{
        "audioOut", talas::core::PortType::Audio, "osc", "audioOut"}};
    document.soundDefinitions.push_back(std::move(input));
    document.virtualControls.push_back(
        talas::project::makeDefaultVirtualControl(
            "LocalLevel", talas::expr::ValueDomain::Scalar));
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"mapperA", "talas.midiMapper", {
            map("!LocalLevel is: (`MIDIController18 min: 0.2 max: 0.8)"),
            choice("voiceModel", "polyphonicInput"),
            expression("channel", "2"),
            expression("polyphony", "1"),
            expression("lowPitch", "0 nn"),
            expression("highPitch", "127 nn"),
        }},
        // Shared, so both Mappers play the key.
        {"mapperB", "talas.midiMapper", {
            map("!LocalLevel is: (`MIDIController19 min: 0.4 max: 1.0)"),
            choice("voiceModel", "polyphonicInput"),
            expression("channel", "2"),
            expression("polyphony", "1"),
            expression("lowPitch", "0 nn"),
            expression("highPitch", "127 nn"),
            shared(),
        }},
        {"sourceA", "", {}, "rangedMapInput"},
        {"sourceB", "", {}, "rangedMapInput"},
    };
    document.graph.connections = {
        {"sourceA", "audioOut", "mapperA", "audioIn"},
        {"sourceB", "audioOut", "mapperB", "audioIn"},
        {"mapperA", "leftOut", "out", "audioIn"},
        {"mapperB", "leftOut", "out", "rightIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    const auto replaced = projectBridge_.editor().replaceDocument(document);
    if (!replaced.ok())
        return finish("ranged local Map project was rejected: "
            + replaced.message);
    documentChanged();
    const auto beforeA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto beforeB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!beforeA || !beforeB
        || beforeA->sourceEventName != "cc18"
        || beforeA->minimum != 0.2 || beforeA->maximum != 0.8
        || beforeA->rangeText != "cc18: 0.200 to 0.800"
        || beforeB->sourceEventName != "cc19"
        || beforeB->minimum != 0.4 || beforeB->maximum != 1.0
        || beforeB->rangeText != "cc19: 0.400 to 1.000"
        || beforeA->enabled || beforeB->enabled)
    {
        return finish("same-named Mapper instances lost their distinct local fader ranges or stage state");
    }

    talas::runtime::RenderSettings settings{1000.0, 64};
    settings.defaultMidiChannel = 2;
    if (!stageProject(std::move(settings), "ranged local Map smoke"))
        return finish("ranged local Map Workbench stage failed: "
            + statusLabel_.getText().toStdString());
    const auto stagedA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto stagedB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!stagedA || !stagedB || !stagedA->enabled || !stagedB->enabled
        || std::abs(stagedA->value - 0.2) > 1.0e-12
        || std::abs(stagedB->value - 0.4) > 1.0e-12)
    {
        return finish("staged local Map faders did not show their independent initial minima");
    }

    projectBridge_.setPlaying(true);
    std::array<float, 64> left{};
    std::array<float, 64> right{};
    std::array<float*, 2> channels{left.data(), right.data()};
    const auto energy = [](const auto& samples) {
        double result = 0.0;
        for (const auto sample : samples)
            result += std::abs(sample);
        return result;
    };
    if (!projectBridge_.render(
            {}, 10.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 10.0)
        || !projectBridge_.render(
            {}, 10.064, channels.data(), channels.size(), left.size()))
    {
        return finish("ranged local Map baseline could not render");
    }
    const auto baselineLeft = energy(left);
    const auto baselineRight = energy(right);
    if (baselineLeft <= 0.0 || baselineRight <= 0.0
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "LocalLevel", 0.8)
        || !projectBridge_.render(
            {}, 10.128, channels.data(), channels.size(), left.size()))
    {
        return finish("first local Map fader did not reach its prepared source");
    }
    const auto movedLeft = energy(left);
    const auto unchangedRight = energy(right);
    if (movedLeft < baselineLeft * 2.5
        || std::abs(unchangedRight - baselineRight)
            > baselineRight * 0.3
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperB", "LocalLevel", 1.0)
        || !projectBridge_.render(
            {}, 10.192, channels.data(), channels.size(), left.size()))
    {
        return finish("first local Map gesture changed the wrong Mapper Input");
    }
    const auto unchangedLeft = energy(left);
    const auto movedRight = energy(right);
    if (std::abs(unchangedLeft - movedLeft) > movedLeft * 0.3
        || movedRight < unchangedRight * 2.0
        || !projectBridge_.enqueueMidiControlChange(2, 18, 0, 10.256)
        || !virtualControlSurface_->applyMidiController(
            {2, 18, 0}, "smoke-midi-input", {}, true)
        || !projectBridge_.render(
            {}, 10.256, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 10.320, channels.data(), channels.size(), left.size()))
    {
        return finish("second local Map gesture or physical CC feedback failed");
    }
    const auto feedbackA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto feedbackB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!feedbackA || !feedbackB
        || std::abs(feedbackA->value - 0.2) > 1.0e-12
        || std::abs(feedbackB->value - 1.0) > 1.0e-12
        || energy(left) > unchangedLeft * 0.4
        || std::abs(energy(right) - movedRight) > movedRight * 0.3)
    {
        return finish("CC18 feedback changed the wrong displayed fader or Input audio: A="
            + std::to_string(feedbackA ? feedbackA->value : -1.0)
            + ", B="
            + std::to_string(feedbackB ? feedbackB->value : -1.0)
            + ", left=" + std::to_string(energy(left))
            + "/" + std::to_string(unchangedLeft)
            + ", right=" + std::to_string(energy(right))
            + "/" + std::to_string(movedRight));
    }
    talas::runtime::RenderSettings restageSettings{1000.0, 64};
    restageSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(restageSettings),
            "ranged local Map restage smoke"))
    {
        return finish("ranged local Map could not restage after live CC");
    }
    const auto restagedA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto restagedB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!restagedA || !restagedB
        || !restagedA->enabled || !restagedB->enabled
        || std::abs(restagedA->value - 0.2) > 1.0e-12
        || std::abs(restagedB->value - 0.4) > 1.0e-12)
    {
        return finish("local Map faders retained stale CC values across a new stage");
    }
    if (!projectBridge_.render(
            {}, 10.384, channels.data(), channels.size(), left.size()))
    {
        return finish("ranged local Map restage could not activate its pending graph");
    }
    for (std::size_t block = 1;
         block < 16 && projectBridge_.snapshotTransitionActive();
         ++block)
    {
        if (!projectBridge_.render(
                {}, 10.384 + static_cast<double>(block) * 0.064,
                channels.data(), channels.size(), left.size()))
        {
            return finish("ranged local Map restage transition could not render");
        }
    }
    if (projectBridge_.snapshotTransitionActive())
        return finish("ranged local Map restage transition did not complete");
    (void)projectBridge_.collectRetired();

    auto sharedSourceDocument = document;
    sharedSourceDocument.graph.nodes[1].parameters[0].text =
        "!LocalLevel is: (`MIDIController07 min: 0.2 max: 0.8)";
    sharedSourceDocument.graph.nodes[2].parameters[0].text =
        "!LocalLevel is: (`MIDIController7 min: 0.4 max: 1.0)";
    if (!projectBridge_.editor().replaceDocument(sharedSourceDocument).ok())
        return finish("shared-source local Map project was rejected");
    documentChanged();
    const auto sharedA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto sharedB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!sharedA || !sharedB
        || sharedA->sourceEventName != "cc07"
        || sharedB->sourceEventName != "cc07"
        || sharedA->minimum != 0.2 || sharedA->maximum != 0.8
        || sharedB->minimum != 0.4 || sharedB->maximum != 1.0)
    {
        return finish("padded and unpadded CC7 local Maps lost their separate fader ranges");
    }
    talas::runtime::RenderSettings sharedSettings{1000.0, 64};
    sharedSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(sharedSettings),
            "shared-source local Map smoke"))
    {
        return finish("shared-source local Map stage failed: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 12.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 12.0)
        || !projectBridge_.render(
            {}, 12.064, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 12.128, channels.data(), channels.size(), left.size())
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "LocalLevel", 0.5))
    {
        return finish("shared-source local Map baseline or first gesture failed");
    }
    const auto sharedAfterA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!sharedAfterA
        || std::abs(sharedAfterA->value - 0.7) > 1.0e-12
        || !projectBridge_.render(
            {}, 12.192, channels.data(), channels.size(), left.size())
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.5 / 0.7) > 0.15)
    {
        return finish("CC7 gesture on the first fader did not update both local projections");
    }
    if (!virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperB", "LocalLevel", 1.0))
    {
        return finish("shared-source second fader gesture was rejected");
    }
    const auto sharedAfterB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!sharedAfterB
        || std::abs(sharedAfterB->value - 0.8) > 1.0e-12
        || !projectBridge_.render(
            {}, 12.256, channels.data(), channels.size(), left.size())
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.8) > 0.15)
    {
        return finish("CC7 gesture on the second fader did not update both local projections");
    }
    if (!projectBridge_.enqueueMidiControlChange(2, 7, 0, 12.320)
        || !virtualControlSurface_->applyMidiController(
            {2, 7, 0}, "smoke-midi-input", {}, true)
        || !projectBridge_.render(
            {}, 12.320, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 12.384, channels.data(), channels.size(), left.size()))
    {
        return finish("shared CC7 physical feedback did not reach both local Maps");
    }
    const auto sharedFeedbackA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto sharedFeedbackB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!sharedFeedbackA || !sharedFeedbackB
        || std::abs(sharedFeedbackA->value - 0.2) > 1.0e-12
        || std::abs(sharedFeedbackB->value - 0.4) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.5) > 0.15)
    {
        return finish("shared CC7 feedback lost the independent Map scales");
    }
    (void)projectBridge_.collectRetired();

    auto taggedRangeDocument = document;
    taggedRangeDocument.graph.nodes[1].parameters[0].text =
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 0.2 max: 0.8)";
    if (!projectBridge_.editor().replaceDocument(taggedRangeDocument).ok())
        return finish("channel and range local Map project was rejected");
    documentChanged();
    const auto taggedBefore = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!taggedBefore || taggedBefore->sourceEventName != "cc07_ch05"
        || taggedBefore->minimum != 0.2 || taggedBefore->maximum != 0.8
        || taggedBefore->rangeText.find("Ch 5") == std::string::npos)
    {
        return finish("channel and range local Map lost its scoped VCS fader");
    }
    talas::runtime::RenderSettings taggedRangeSettings{1000.0, 64};
    taggedRangeSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(taggedRangeSettings),
            "channel and range local Map smoke"))
    {
        return finish("channel and range local Map stage failed: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 13.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 13.0)
        || !projectBridge_.render(
            {}, 13.064, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 13.128, channels.data(), channels.size(), left.size())
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "LocalLevel", 0.5)
        || !projectBridge_.render(
            {}, 13.192, channels.data(), channels.size(), left.size()))
    {
        return finish("channel and range local fader gesture failed");
    }
    const auto taggedAfterGesture = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!taggedAfterGesture
        || std::abs(taggedAfterGesture->value - 0.5) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.5 / 0.4) > 0.3
        || projectBridge_.enqueueMidiControlChange(2, 7, 127, 13.225)
        || !projectBridge_.enqueueMidiControlChange(5, 7, 127, 13.256)
        || !virtualControlSurface_->applyMidiController(
            {5, 7, 127}, "smoke-midi-input", {}, true)
        || !projectBridge_.render(
            {}, 13.256, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 13.320, channels.data(), channels.size(), left.size()))
    {
        return finish("channel-tagged CC7 failed to override the ranged local fader");
    }
    const auto taggedAfterMidi = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    const auto untaggedAfterMidi = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "LocalLevel");
    if (!taggedAfterMidi || !untaggedAfterMidi
        || std::abs(taggedAfterMidi->value - 0.8) > 1.0e-12
        || std::abs(untaggedAfterMidi->value - 0.4) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.8 / 0.4) > 0.3)
    {
        return finish("channel-tagged physical CC changed the wrong local Map");
    }
    (void)projectBridge_.collectRetired();

    const std::array displayTypes{
        std::pair{"#fader", "fader"},
        std::pair{"#smallFader", "smallFader"},
        std::pair{"#toggle", "toggle"},
        std::pair{"#gate", "gate"},
        std::pair{"#nothing", "nothing"},
    };
    for (std::size_t index = 0; index < displayTypes.size(); ++index)
    {
        const auto& [tag, type] = displayTypes[index];
        auto displayDocument = document;
        displayDocument.graph.nodes[1].parameters[0].text =
            std::string("!LocalLevel is: (`MIDIController07 displayAs: ")
                + tag + ")";
        if (!projectBridge_.editor().replaceDocument(displayDocument).ok())
            return finish("displayAs local Map project was rejected");
        documentChanged();
        const auto before = virtualControlSurface_
            ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
        if ((std::string_view{tag} == "#nothing" && before)
            || (std::string_view{tag} != "#nothing"
                && (!before || before->displayType != type
                    || before->sourceEventName != "cc07"
                    || before->minimum != 0.0 || before->maximum != 1.0
                    || before->enabled)))
        {
            return finish("displayAs local Map chose the wrong VCS control before stage");
        }
        talas::runtime::RenderSettings displaySettings{1000.0, 64};
        displaySettings.defaultMidiChannel = 2;
        if (!stageProject(std::move(displaySettings),
                "displayAs local Map smoke"))
        {
            return finish("displayAs local Map stage failed: "
                + statusLabel_.getText().toStdString());
        }
        const auto staged = virtualControlSurface_
            ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
        if ((std::string_view{tag} == "#nothing" && staged)
            || (std::string_view{tag} != "#nothing"
                && (!staged || !staged->enabled
                    || staged->displayType != type)))
        {
            return finish("displayAs local Map chose the wrong staged VCS control");
        }
        projectBridge_.setPlaying(true);
        const auto at = 14.0 + static_cast<double>(index);
        if (!projectBridge_.render(
                {}, at, channels.data(), channels.size(), left.size())
            || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, at)
            || !projectBridge_.render(
                {}, at + 0.064, channels.data(), channels.size(), left.size()))
        {
            return finish("displayAs local Map could not start its Input voice");
        }
        if (std::string_view{tag} == "#nothing")
        {
            if (!projectBridge_.enqueueMidiControlChange(2, 7, 127,
                    at + 0.096))
                return finish("hidden local Map rejected its physical CC source");
        }
        else if (std::string_view{tag} == "#toggle"
            || std::string_view{tag} == "#gate")
        {
            if (!virtualControlSurface_->operateLocalMapButtonForSmoke(
                    "mapperA", "LocalLevel", true)
                || virtualControlSurface_->setLocalMapValueForSmoke(
                    "mapperA", "LocalLevel", 0.5))
            {
                return finish("binary displayAs control did not enforce its VCS gesture");
            }
        }
        else if (!virtualControlSurface_->setLocalMapValueForSmoke(
                "mapperA", "LocalLevel", 0.5))
        {
            return finish("displayAs fader gesture was rejected");
        }
        if (!projectBridge_.render(
                {}, at + 0.128, channels.data(), channels.size(), left.size())
            || energy(left) <= 0.0)
        {
            return finish("displayAs control did not reach its Input audio");
        }
        if (std::string_view{tag} == "#toggle"
            || std::string_view{tag} == "#gate")
        {
            const auto deactivate =
                std::string_view{tag} == "#toggle";
            if (!virtualControlSurface_->operateLocalMapButtonForSmoke(
                    "mapperA", "LocalLevel", deactivate)
                || !projectBridge_.render(
                    {}, at + 0.192, channels.data(), channels.size(),
                    left.size())
                || energy(left) > 1.0e-12)
            {
                return finish("binary displayAs control did not release its Input");
            }
        }
        (void)projectBridge_.collectRetired();
    }

    auto gridDocument = document;
    gridDocument.graph.nodes[1].parameters[0].text =
        "!LocalLevel is: (`MIDIController07 min: 0.5 max: 0.75 grid: 0.05)";
    if (!projectBridge_.editor().replaceDocument(gridDocument).ok())
        return finish("gridded local Map project was rejected");
    documentChanged();
    const auto gridBefore = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!gridBefore || gridBefore->sourceEventName != "cc07"
        || gridBefore->minimum != 0.5 || gridBefore->maximum != 0.75
        || gridBefore->rangeText.find("grid 0.050") == std::string::npos)
    {
        return finish("gridded local Map lost its visible step and range");
    }
    talas::runtime::RenderSettings gridSettings{1000.0, 64};
    gridSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(gridSettings), "gridded local Map smoke"))
        return finish("gridded local Map stage failed: "
            + statusLabel_.getText().toStdString());
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 20.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 20.0)
        || !projectBridge_.render(
            {}, 20.064, channels.data(), channels.size(), left.size())
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "LocalLevel", 0.63)
        || !projectBridge_.render(
            {}, 20.128, channels.data(), channels.size(), left.size()))
    {
        return finish("gridded local Map fader gesture failed");
    }
    const auto gridAfterGesture = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!gridAfterGesture
        || std::abs(gridAfterGesture->value - 0.65) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.65 / 0.4) > 0.3
        || !projectBridge_.enqueueMidiControlChange(2, 7, 0, 20.160)
        || !virtualControlSurface_->applyMidiController(
            {2, 7, 0}, "smoke-midi-input", {}, true)
        || !projectBridge_.render(
            {}, 20.192, channels.data(), channels.size(), left.size()))
    {
        return finish("gridded local Map gesture or PCM missed its step");
    }
    const auto gridAfterMidi = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!gridAfterMidi
        || std::abs(gridAfterMidi->value - 0.5) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.5 / 0.4) > 0.3)
    {
        return finish("gridded physical CC feedback missed the minimum step");
    }
    (void)projectBridge_.collectRetired();

    auto combinedDocument = document;
    combinedDocument.graph.nodes[1].parameters[0].text =
        "!LocalLevel is: (`MIDIController07 channel: 5; min: 0.1 max: 1 grid: 0.05; taper: #log; displayAs: #smallFader)";
    if (!projectBridge_.editor().replaceDocument(combinedDocument).ok())
        return finish("combined log Map project was rejected");
    documentChanged();
    const auto combinedBefore = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!combinedBefore || combinedBefore->sourceEventName != "cc07_ch05"
        || combinedBefore->displayType != "smallFader"
        || combinedBefore->minimum != 0.1
        || combinedBefore->maximum != 1.0
        || combinedBefore->rangeText.find("Ch 5") == std::string::npos
        || combinedBefore->rangeText.find("grid 0.050") == std::string::npos
        || combinedBefore->rangeText.find("log") == std::string::npos)
    {
        return finish("combined log Map lost its scoped fader and option labels");
    }
    talas::runtime::RenderSettings combinedSettings{1000.0, 64};
    combinedSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(combinedSettings),
            "combined log local Map smoke"))
    {
        return finish("combined log Map stage failed: "
            + statusLabel_.getText().toStdString());
    }
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 21.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 21.0)
        || !projectBridge_.render(
            {}, 21.064, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 21.128, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiControlChange(5, 7, 64, 21.192)
        || !virtualControlSurface_->applyMidiController(
            {5, 7, 64}, "smoke-midi-input", {}, true)
        || !projectBridge_.render(
            {}, 21.192, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 21.256, channels.data(), channels.size(), left.size()))
    {
        return finish("combined log Map could not route channel-tagged CC audio");
    }
    const auto combinedMidi = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!combinedMidi || std::abs(combinedMidi->value - 0.3) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.3 / 0.4) > 0.3
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "LocalLevel", 0.55)
        || !projectBridge_.render(
            {}, 21.320, channels.data(), channels.size(), left.size()))
    {
        return finish("combined log Map feedback or inverse fader mapping failed");
    }
    const auto combinedGesture = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "LocalLevel");
    if (!combinedGesture
        || std::abs(combinedGesture->value - 0.55) > 1.0e-12
        || energy(right) <= 0.0
        || std::abs(energy(left) / energy(right) - 0.55 / 0.4) > 0.3)
    {
        return finish("combined log Map fader and prepared PCM diverged");
    }
    (void)projectBridge_.collectRetired();

    auto frequencyDocument = document;
    auto& oscillator =
        frequencyDocument.soundDefinitions.front().graph.nodes.front();
    oscillator.parameters[0].expression = "!Frequency";
    oscillator.parameters[1].expression = "!KeyDown > 0";
    frequencyDocument.virtualControls = {
        talas::project::makeDefaultVirtualControl(
            "Frequency", talas::expr::ValueDomain::Hertz)};
    frequencyDocument.graph.nodes[1].parameters[0].text =
        "!Frequency is: (`MIDIController18 min: 100 max: 1000)";
    frequencyDocument.graph.nodes[2].parameters[0].text =
        "!Frequency is: (`MIDIController19 min: 200 max: 1200)";
    if (!projectBridge_.editor().replaceDocument(frequencyDocument).ok())
        return finish("ranged Hertz Map project was rejected");
    documentChanged();
    const auto hertzA = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperA", "Frequency");
    const auto hertzB = virtualControlSurface_
        ->localMapControlPresentationForSmoke("mapperB", "Frequency");
    if (!hertzA || !hertzB
        || hertzA->rangeText != "cc18: 100.000 to 1000.000"
        || hertzB->rangeText != "cc19: 200.000 to 1200.000")
    {
        return finish("ranged Hertz Map did not expose two distinct frequency faders");
    }
    talas::runtime::RenderSettings hertzSettings{4000.0, 64};
    hertzSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(hertzSettings),
            "ranged Hertz local Map smoke"))
    {
        return finish("ranged Hertz Map stage failed: "
            + statusLabel_.getText().toStdString());
    }
    const auto crossings = [](const auto& samples) {
        int result = 0;
        int previousSign = 0;
        for (const auto sample : samples)
        {
            const auto sign = sample > 1.0e-4F ? 1
                : sample < -1.0e-4F ? -1 : 0;
            if (sign != 0)
            {
                if (previousSign != 0 && sign != previousSign)
                    ++result;
                previousSign = sign;
            }
        }
        return result;
    };
    projectBridge_.setPlaying(true);
    if (!projectBridge_.render(
            {}, 20.0, channels.data(), channels.size(), left.size())
        || !projectBridge_.enqueueMidiNoteOn(2, 60, 127, 20.0)
        || !projectBridge_.render(
            {}, 20.016, channels.data(), channels.size(), left.size())
        || !projectBridge_.render(
            {}, 20.032, channels.data(), channels.size(), left.size()))
    {
        return finish("ranged Hertz Map baseline could not render");
    }
    const auto minimumLeftCrossings = crossings(left);
    const auto initialRightCrossings = crossings(right);
    const auto midpointAccepted = virtualControlSurface_
        ->setLocalMapValueForSmoke("mapperA", "Frequency", 550.0);
    const auto midpointRendered = midpointAccepted && projectBridge_.render(
        {}, 20.048, channels.data(), channels.size(), left.size());
    if (minimumLeftCrossings < 2 || minimumLeftCrossings > 5
        || initialRightCrossings < 5 || initialRightCrossings > 8
        || !midpointAccepted || !midpointRendered)
    {
        return finish("ranged Hertz Map minima or midpoint gesture failed: left="
            + std::to_string(minimumLeftCrossings)
            + ", right=" + std::to_string(initialRightCrossings)
            + ", gesture=" + std::to_string(midpointAccepted)
            + ", render=" + std::to_string(midpointRendered));
    }
    const auto midpointLeftCrossings = crossings(left);
    const auto unchangedRightAtMidpoint = crossings(right);
    if (midpointLeftCrossings < 14 || midpointLeftCrossings > 20
        || std::abs(unchangedRightAtMidpoint - initialRightCrossings) > 2
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperA", "Frequency", 1000.0)
        || !projectBridge_.render(
            {}, 20.064, channels.data(), channels.size(), left.size()))
    {
        return finish("ranged Hertz Map midpoint did not change only its Input pitch: left="
            + std::to_string(midpointLeftCrossings)
            + ", right=" + std::to_string(unchangedRightAtMidpoint)
            + ", prior right=" + std::to_string(initialRightCrossings));
    }
    const auto maximumLeftCrossings = crossings(left);
    const auto unchangedRightAtMaximum = crossings(right);
    if (maximumLeftCrossings < 29 || maximumLeftCrossings > 34
        || std::abs(unchangedRightAtMaximum - initialRightCrossings) > 2
        || !virtualControlSurface_->setLocalMapValueForSmoke(
            "mapperB", "Frequency", 1200.0)
        || !projectBridge_.render(
            {}, 20.080, channels.data(), channels.size(), left.size()))
    {
        return finish("ranged Hertz Map maximum did not change only its Input pitch: left="
            + std::to_string(maximumLeftCrossings)
            + ", right=" + std::to_string(unchangedRightAtMaximum)
            + ", prior right=" + std::to_string(initialRightCrossings));
    }
    const auto globalFrequency = std::ranges::find(
        projectBridge_.editor().document().virtualControls,
        "Frequency", &talas::project::VirtualControl::eventName);
    if (crossings(right) < 35 || crossings(right) > 41
        || std::abs(crossings(left) - maximumLeftCrossings) > 2
        || globalFrequency
            == projectBridge_.editor().document().virtualControls.end()
        || globalFrequency->value != 440.0)
    {
        return finish("ranged Hertz faders changed the global Frequency or the wrong local pitch: left="
            + std::to_string(crossings(left))
            + ", right=" + std::to_string(crossings(right))
            + ", prior left=" + std::to_string(maximumLeftCrossings)
            + ", global=" + std::to_string(
                globalFrequency
                    == projectBridge_.editor().document().virtualControls.end()
                    ? -1.0 : globalFrequency->value));
    }
    (void)projectBridge_.collectRetired();
    return finish();
}

std::optional<std::string>
WorkbenchComponent::runBuiltInMidiButtonSwitchSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto previousDocumentStaged = currentDocumentStaged_;
    const auto previousTransportPlaying = transportPlaying_;
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        projectBridge_.setPlaying(false);
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        if (previousDocumentStaged)
        {
            talas::runtime::RenderSettings restoreSettings{1000.0, 64};
            if (!stageProject(std::move(restoreSettings),
                    "built-in MIDI VCS smoke restore")
                && error.empty())
                error = "built-in MIDI VCS smoke could not restore the prior stage";
        }
        setPlaying(previousTransportPlaying);
        if (!restored.ok() && error.empty())
            error = "built-in MIDI VCS smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto expression = [](std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.expression = std::move(value);
        return parameter;
    };

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"osc", "talas.oscillator", {
            expression("frequency", "100 hz"),
            expression("amplitude", "(!bt00 + !sw127) / 2"),
        }},
    };
    document.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    const auto discovered =
        talas::project::synchronizeProjectEventValueControls(
            document, registry_);
    if (!discovered.ok() || document.virtualControls.size() != 2)
        return finish("built-in MIDI VCS controls could not be discovered");
    const auto replaced = projectBridge_.editor().replaceDocument(document);
    if (!replaced.ok())
        return finish("built-in MIDI VCS project was rejected: "
            + replaced.message);
    documentChanged();
    const auto gate = virtualControlSurface_
        ->controlPresentationForSmoke("bt00");
    const auto toggle = virtualControlSurface_
        ->controlPresentationForSmoke("sw127");
    if (!gate || !toggle
        || gate->kind != talas::project::VirtualControlKind::Gate
        || toggle->kind != talas::project::VirtualControlKind::Toggle
        || !gate->expectedWidgetVisible
        || !toggle->expectedWidgetVisible)
        return finish("bt00 momentary button or sw127 checkbox was missing from the VCS");

    talas::runtime::RenderSettings renderSettings{1000.0, 64};
    renderSettings.defaultMidiChannel = 2;
    if (!stageProject(std::move(renderSettings),
            "built-in MIDI VCS smoke"))
        return finish("built-in MIDI VCS Sound could not stage");
    projectBridge_.setPlaying(true);
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    const auto render = [&](double callbackTime) {
        return projectBridge_.render({}, callbackTime,
            channels.data(), channels.size(), output.size());
    };
    const auto silent = [&] {
        return std::ranges::all_of(output, [](float sample) {
            return std::abs(sample) < 1.0e-6F;
        });
    };
    const auto audible = [&] {
        return std::ranges::any_of(output, [](float sample) {
            return std::abs(sample) > 0.4F;
        });
    };
    if (!render(10.0) || !silent()
        || !virtualControlSurface_->operateButtonForSmoke("bt00", true)
        || !render(10.064) || !audible()
        || !virtualControlSurface_->operateButtonForSmoke("bt00", false)
        || !render(10.128) || !silent())
        return finish("bt00 momentary press and release did not control Sound audio");
    if (!virtualControlSurface_->operateButtonForSmoke("sw127", true)
        || !render(10.192) || !audible()
        || !virtualControlSurface_->operateButtonForSmoke("sw127", true)
        || !render(10.256) || !silent())
        return finish("sw127 checkbox did not alternate Sound audio");

    if (!virtualControlSurface_->applyMidiController(
            {2, 0, 127}, "smoke-midi-input", {}, true)
        || !virtualControlSurface_->applyMidiController(
            {2, 127, 127}, "smoke-midi-input", {}, true))
        return finish("built-in CC feedback did not reach button and checkbox rows");
    const auto gateOn = virtualControlSurface_
        ->controlPresentationForSmoke("bt00");
    const auto toggleOn = virtualControlSurface_
        ->controlPresentationForSmoke("sw127");
    if (!gateOn || !toggleOn || gateOn->value != 1.0
        || toggleOn->value != 1.0)
        return finish("built-in CC feedback did not show both controls on");
    if (!virtualControlSurface_->applyMidiController(
            {2, 0, 0}, "smoke-midi-input", {}, true)
        || !virtualControlSurface_->applyMidiController(
            {2, 127, 0}, "smoke-midi-input", {}, true))
        return finish("built-in CC release feedback was rejected");
    const auto gateOff = virtualControlSurface_
        ->controlPresentationForSmoke("bt00");
    const auto toggleOff = virtualControlSurface_
        ->controlPresentationForSmoke("sw127");
    if (!gateOff || !toggleOff || gateOff->value != 0.0
        || toggleOff->value != 0.0)
        return finish("built-in CC feedback did not show both controls off");
    (void)projectBridge_.collectRetired();
    return finish();
}
}
