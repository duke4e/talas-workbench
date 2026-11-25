#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/WorkspaceSessionFile.h"
#include "talas/project/WorkspaceSessionState.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-standalone-workflow-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "fixture file should be readable");
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(source);
    return result;
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value};
}

talas::project::ProjectSoundDefinition oscillatorDefinition(
    std::string definitionId,
    double frequency)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = definition.definitionId;
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            expression("frequency", std::to_string(frequency) + " hz"),
            expression("amplitude", "!Gain"),
        },
    });
    talas::project::ProjectSoundExposedParameter gain;
    gain.parameterId = "gain";
    gain.displayName = "Gain";
    gain.defaultValue = expression("gain", "0.2");
    gain.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::EventValue,
        {},
        {},
        "Gain",
        talas::expr::ValueDomain::Scalar,
    });
    definition.exposedParameters.push_back(std::move(gain));
    return definition;
}

talas::project::ProjectSoundDefinition nestedSoundDefinition(
    std::string definitionId,
    std::string childDefinitionId)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = definition.definitionId;
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "nestedVoice",
        "audioOut",
    });
    talas::project::ProjectNode child;
    child.instanceId = "nestedVoice";
    child.soundDefinitionId = std::move(childDefinitionId);
    definition.graph.nodes.push_back(std::move(child));
    return definition;
}

talas::project::ProjectDocument standaloneDocument()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Local Standalone Workflow";
    document.timeline.musicalTime.tempo = {{0, 500000}};
    document.soundDefinitions = {
        nestedSoundDefinition("voiceA", "voiceB"),
        oscillatorDefinition("voiceB", 330.0),
    };

    talas::project::ProjectNode source;
    source.instanceId = "voiceSource";
    source.soundDefinitionId = "voiceA";
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"gain", "talas.constant", {expression("value", "!Gain")}},
        std::move(source),
    };
    document.graph.connections = {
        {"voiceSource", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };

    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.domain = talas::expr::ValueDomain::Scalar;
    gain.kind = talas::project::VirtualControlKind::Fader;
    gain.value = 0.5;
    gain.minimum = 0.0;
    gain.maximum = 1.0;
    gain.midiMapping = talas::project::MidiControlMapping{
        1,
        74,
        talas::project::MidiControlSourceMode::ExactInput,
        "midi-device",
    };
    gain.oscMapping = talas::project::OscControlMapping{"/gain"};
    gain.oscEgressMapping =
        talas::project::OscEgressMapping{"/feedback/gain"};
    document.virtualControls.push_back(std::move(gain));

    document.variableLibrary.variables.push_back({
        .variableId = "ProjectGain",
        .displayName = "Project Gain",
        .domain = talas::expr::ValueDomain::Scalar,
        .initialValue = 0.25,
        .owner = {talas::project::ProjectVariableOwnerKind::Project},
        .snapshotEligible = true,
    });
    document.variableLibrary.variables.push_back({
        .variableId = "SessionMacro",
        .displayName = "Session Macro",
        .domain = talas::expr::ValueDomain::Scalar,
        .initialValue = 0.0,
        .owner = {talas::project::ProjectVariableOwnerKind::WorkspaceSession},
        .snapshotEligible = false,
    });
    document.soundVariableLibrary.variables.push_back({
        .variableId = "SessionVoice",
        .displayName = "Session Voice",
        .owner = {talas::project::ProjectVariableOwnerKind::WorkspaceSession},
        .kind = talas::project::ProjectSoundVariableKind::SoundCollection,
        .candidateDefinitionIds = {"voiceA", "voiceB"},
        .initialSelectedOrdinal = 0,
        .snapshotEligible = false,
    });

    talas::project::SnapshotTargetIdentity gainTarget;
    gainTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    gainTarget.eventName = "Gain";
    gainTarget.domain = talas::expr::ValueDomain::Scalar;
    talas::project::SnapshotValue halfGain;
    halfGain.kind = talas::project::SnapshotValueKind::Number;
    halfGain.domain = talas::expr::ValueDomain::Scalar;
    halfGain.number = 0.5;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "performance";
    bank.displayName = "Performance";
    bank.snapshots.push_back({
        "half",
        "Half Gain",
        {{gainTarget, halfGain}},
    });
    document.snapshotLibrary.banks.push_back(std::move(bank));

    document.timeline.score.tracks = {
        {
            "voice",
            "Voice",
            talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::MusicalTicks,
            "master",
            1,
            talas::project::ScoreOverlapMode::Layer,
        },
        {
            "master",
            "Master",
            talas::project::ScoreTrackKind::Master,
            talas::project::ScoreTimeDomain::Seconds,
            {},
            1,
            talas::project::ScoreOverlapMode::Layer,
        },
    };
    document.timeline.score.clips.push_back({
        "introClip",
        "voice",
        "voiceB",
        ticks(0),
        ticks(96),
        ticks(0),
        ticks(0),
        ticks(0),
        1,
    });
    document.timeline.score.automation.push_back({
        "gainLane",
        talas::project::ScoreAutomationScope::Track,
        "voice",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {
            {ticks(0), 0.25, talas::core::AutomationInterpolation::Linear},
            {ticks(96), 0.75, talas::core::AutomationInterpolation::Step},
        },
    });
    document.timeline.score.markers.push_back({
        "intro",
        "Intro",
        ticks(0),
        ticks(192),
    });

    document.assets.push_back({
        "Library/MissingTable",
        talas::project::ProjectAssetKind::Table,
        "assets/missing-table.txt",
        talas::project::ProjectAssetDisposition::Referenced,
        {},
        0,
        0.0,
        0,
        {},
        1,
        "owner-local unresolved preservation fixture",
    });

    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "main";
    liveSet.displayName = "Main";
    talas::project::ProjectLiveSetCell soundCell;
    soundCell.cellId = "sound";
    soundCell.displayName = "Sound";
    soundCell.target = talas::project::ProjectLiveSetSoundTarget{
        "voiceA",
        std::string{"voiceSource"},
    };
    soundCell.mappings.push_back(
        talas::project::ProjectLiveSetWorkbenchMapping{});
    talas::project::ProjectLiveSetCell snapshotCell;
    snapshotCell.cellId = "snapshot";
    snapshotCell.displayName = "Snapshot";
    snapshotCell.column = 1;
    snapshotCell.target = talas::project::ProjectLiveSetSnapshotTarget{
        "performance",
        "half",
    };
    snapshotCell.mappings.push_back(talas::project::ProjectLiveSetOscMapping{
        "/live/snapshot",
    });
    talas::project::ProjectLiveSetCell regionCell;
    regionCell.cellId = "region";
    regionCell.displayName = "Region";
    regionCell.column = 2;
    regionCell.target =
        talas::project::ProjectLiveSetScoreRegionTarget{"intro"};
    regionCell.mappings.push_back(
        talas::project::ProjectLiveSetMidiProgramMapping{
            1,
            7,
            "midi-device",
        });
    talas::project::ProjectLiveSetCell stopCell;
    stopCell.cellId = "stop";
    stopCell.displayName = "Stop";
    stopCell.column = 3;
    stopCell.target = talas::project::ProjectLiveSetStopTarget{};
    stopCell.mappings.push_back(talas::project::ProjectLiveSetOscMapping{
        "/live/stop",
    });
    liveSet.cells = {
        std::move(soundCell),
        std::move(snapshotCell),
        std::move(regionCell),
        std::move(stopCell),
    };
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));

    talas::project::ProjectLiveTool tool;
    tool.toolId = "conductor";
    tool.displayName = "Conductor";
    tool.initialStateId = "idle";
    tool.states = {{"idle", "Idle"}, {"armed", "Armed"}};
    tool.variables.push_back({
        "enabled",
        "Enabled",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    talas::project::ProjectLiveToolTransition arm;
    arm.transitionId = "arm";
    arm.fromStateId = "idle";
    arm.toStateId = "armed";
    arm.trigger.kind = talas::project::ProjectLiveToolTriggerKind::VariableChange;
    arm.trigger.variableId = "enabled";
    arm.guardExpression = "@enabled";
    arm.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::ActivateCell,
        "main",
        "snapshot",
    });
    tool.transitions.push_back(std::move(arm));
    document.liveSetLibrary.tools.push_back(std::move(tool));

    talas::project::reconcileProjectPresentation(document);
    return document;
}

talas::project::WorkspaceSessionState standaloneWorkspace(
    const talas::project::ProjectDocument& document)
{
    talas::project::WorkspaceSessionState state;
    state.viewMode = talas::project::WorkspaceViewMode::Performance;
    state.activeSoundDefinitionId = "voiceA";
    state.structureNavigationPath = {"voiceA"};
    state.graphContexts = {
        {std::nullopt, 12.0, -4.0, 1.1, "voiceSource"},
        {"voiceA", 24.0, 8.0, 1.25, "osc"},
    };
    state.focusedPerformanceEventName = "Gain";
    state.performance.selectedControlId = "Gain";
    state.performance.selectedLiveSetId = "main";
    state.performance.selectedLiveSetCellId = "snapshot";
    state.performance.selectedLiveToolId = "conductor";
    state.performance.mode =
        talas::project::WorkspacePerformanceSessionState::Mode::LiveGrid;
    state.performance.liveToolStates.push_back({
        "conductor",
        "armed",
        talas::project::workspaceSessionLiveToolDefinitionFingerprint(
            document.liveSetLibrary.tools.front()),
        {{
            "enabled",
            talas::project::ProjectLiveToolVariableKind::Boolean,
            0.0,
            true,
        }},
    });
    state.accessibility.interfaceScale =
        talas::project::WorkspaceInterfaceScale::Percent130;
    state.accessibility.enhancedContrast = true;
    state.accessibility.reduceMotion = true;
    state.accessibility.contextHelpVisible = true;
    state.variableValues.push_back({
        "SessionMacro",
        talas::expr::ValueDomain::Scalar,
        0.75,
        talas::project::workspaceSessionVariableDeclarationFingerprint(
            document.variableLibrary.variables[1]),
    });
    state.soundVariableValues.push_back({
        "SessionVoice",
        "voiceB",
        talas::project::workspaceSessionSoundVariableDeclarationFingerprint(
            document.soundVariableLibrary.variables.front()),
    });
    talas::project::reconcileWorkspaceSessionState(state, document);
    return state;
}

talas::runtime::RenderSettings renderSettings(double sampleRate = 48000.0)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = 64;
    settings.eventValues.push_back({
        "Gain",
        0.5,
        talas::expr::ValueDomain::Scalar,
    });
    return settings;
}

bool renderBridge(
    talas::runtime::ProjectRuntimeBridge& bridge,
    std::array<float, 64>& output)
{
    output.fill(0.0F);
    std::array<float*, 1> channels{output.data()};
    return bridge.render(channels.data(), channels.size(), output.size());
}

bool hasAudibleSample(const auto& samples)
{
    return std::ranges::any_of(samples, [](auto sample) {
        return std::abs(sample) > 1.0e-6;
    });
}

talas::project::ProjectDocument missingComponentReplacement(
    talas::project::ProjectDocument document)
{
    talas::project::ProjectNode missing;
    missing.instanceId = "osc";
    missing.typeId = "org.talas.closeout.missing";
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.0.0";
    component.stateSchemaVersion = 1;
    component.state = {0x01, 0x02, 0x03};
    component.displayName = "Preserved Missing Component";
    component.bundleId = "org.talas.closeout.missing";
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        talas::core::defaultPortSignalContract(talas::core::PortType::Audio),
    });
    missing.preservedComponent = std::move(component);
    document.soundDefinitions[1].graph.nodes.front() =
        std::move(missing);
    talas::project::reconcileProjectPresentation(document);
    return document;
}

void testCurrentProjectAndWorkspaceRemainSeparateAndRecoverable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = standaloneDocument();
    require(
        talas::project::validateProjectDocumentForEditing(document, registry).ok(),
        "cumulative current-format document should satisfy editing authority");
    require(
        talas::project::validateProjectLiveSetLibrary(document).ok(),
        "cumulative Live Set and Tool authority should resolve exactly");

    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto parsed = talas::project::parseProjectDocument(canonical);
    require(
        parsed.ok()
            && talas::project::serializeProjectDocument(parsed.document)
                == canonical,
        "format-34 cumulative project should reopen byte-canonically");
    require(
        canonical.find("selectedLiveSetCellId") == std::string::npos
            && canonical.find("liveToolStates") == std::string::npos,
        "Workspace-only live UI/runtime values must not leak into project bytes");

    auto workspace = standaloneWorkspace(document);
    require(
        workspace.performance.selectedLiveSetCellId
                == std::optional<std::string>{"snapshot"}
            && workspace.performance.liveToolStates.size() == 1
            && workspace.variableValues.size() == 1
            && workspace.soundVariableValues.size() == 1,
        "compatible WorkspaceSession identities should survive reconciliation");
    const auto workspaceBytes =
        talas::project::serializeWorkspaceSessionState(workspace);
    const auto parsedWorkspace =
        talas::project::parseWorkspaceSessionState(workspaceBytes);
    require(
        parsedWorkspace.ok() && parsedWorkspace.state == workspace,
        "format-13 WorkspaceSession should round-trip independently");

    TemporaryDirectory temporary;
    const auto projectPath = temporary.path() / "project.talas.json";
    const auto localStateRoot = temporary.path() / "local-state";
    const auto sessionPath = talas::project::workspaceSessionFilePathForProject(
        localStateRoot,
        projectPath);
    require(sessionPath.has_value(),
            "saved project should derive one local WorkspaceSession identity");
    require(talas::project::saveProjectFile(projectPath, document).ok(),
            "cumulative project should save");
    require(talas::project::saveWorkspaceSessionFile(*sessionPath, workspace).ok(),
            "cumulative local WorkspaceSession should save separately");
    const auto projectBytesBeforeSessionEdit = readText(projectPath);

    workspace.performance.panX = 96.0;
    workspace.variableValues.front().value = 0.875;
    require(talas::project::saveWorkspaceSessionFile(*sessionPath, workspace).ok(),
            "WorkspaceSession-only mutation should save");
    require(readText(projectPath) == projectBytesBeforeSessionEdit,
            "WorkspaceSession save must leave exact project bytes unchanged");

    auto backupPath = *sessionPath;
    backupPath += ".bak";
    require(talas::project::saveWorkspaceSessionFile(backupPath, workspace).ok(),
            "recovery fixture should publish a valid session backup");
    std::error_code removalError;
    std::filesystem::remove(*sessionPath, removalError);
    require(!removalError,
            "interrupted-replacement fixture should remove its primary session");
    const auto recovered = talas::project::loadWorkspaceSessionFile(*sessionPath);
    require(
        recovered.ok() && recovered.state == workspace,
        "missing local primary should recover its preserved backup");

    {
        std::ofstream corrupt(*sessionPath, std::ios::binary | std::ios::trunc);
        corrupt << R"json({"format":"talas.workspace-session","version":999})json";
    }
    const auto invalid = talas::project::loadWorkspaceSessionFile(*sessionPath);
    const auto reopenedProject = talas::project::loadProjectFile(projectPath);
    require(
        !invalid.ok()
            && invalid.error.has_value()
            && invalid.error->code
                == talas::project::WorkspaceSessionFileErrorCode::InvalidSession
            && invalid.state == talas::project::WorkspaceSessionState{}
            && reopenedProject.ok()
            && talas::project::serializeProjectDocument(*reopenedProject.document)
                == projectBytesBeforeSessionEdit,
        "invalid local primary should fail closed to defaults without blocking or changing the project");

    const auto saveAsPath = temporary.path() / "copy.talas.json";
    require(talas::project::saveProjectFile(saveAsPath, document).ok(),
            "Save As should publish the same current project authority");
    const auto saveAsSessionPath =
        talas::project::workspaceSessionFilePathForProject(
            localStateRoot,
            saveAsPath);
    require(
        saveAsSessionPath.has_value() && *saveAsSessionPath != *sessionPath
            && !std::filesystem::exists(*saveAsSessionPath)
            && readText(saveAsPath) == projectBytesBeforeSessionEdit,
        "Save As should not silently clone local WorkspaceSession state into the new project identity");

    const auto missingRoundTrip = missingComponentReplacement(document);
    const auto missingBytes =
        talas::project::serializeProjectDocument(missingRoundTrip);
    const auto reopenedMissing =
        talas::project::parseProjectDocument(missingBytes);
    require(
        reopenedMissing.ok()
            && reopenedMissing.document.soundDefinitions.front()
                    .graph.nodes.front().soundDefinitionId
                == missingRoundTrip.soundDefinitions.front()
                       .graph.nodes.front().soundDefinitionId
            && reopenedMissing.document.soundDefinitions[1]
                    .graph.nodes.front().preservedComponent
                == missingRoundTrip.soundDefinitions[1]
                       .graph.nodes.front().preservedComponent
            && reopenedMissing.document.assets.front()
                == document.assets.front(),
        "missing component state and unresolved asset identity should survive a canonical reopen without activation");
}

void testOfflineRealtimeReplacementTransportDeviceAndIngressAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto document = standaloneDocument();
    const auto expanded = talas::project::expandProjectSounds(
        document,
        registry);
    require(
        expanded.ok()
            && std::ranges::any_of(
                expanded.manifest.nodes,
                [](const auto& identity) {
                    return identity.instancePath
                        == std::vector<std::string>{
                            "voiceSource", "nestedVoice"};
                }),
        "cumulative fixture should execute through a genuinely nested Sound instance path");
    auto compiled = talas::runtime::compileProject(
        document,
        registry,
        renderSettings());
    require(
        compiled.ok(),
        "cumulative standalone project should compile"
            + (compiled.diagnostics.empty()
                   ? std::string{}
                   : ": " + compiled.diagnostics.front().message));

    talas::runtime::OfflineRenderSession offline(*compiled.graph);
    const auto offlineResult = offline.render(64);
    std::array<double, 64> realtimeOutput{};
    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(
        offlineResult.ok() && realtime.ready()
            && realtime.render(realtimeOutput)
            && hasAudibleSample(offlineResult.mono)
            && std::ranges::equal(
                offlineResult.mono,
                realtimeOutput,
                [](double left, double right) {
                    return std::abs(left - right) < 1.0e-12;
                }),
        "offline and prepared realtime should consume the same nested-Sound/Score project authority");

    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    bridge.setPlaying(true);
    require(bridge.compileAndStage(renderSettings()).ok(),
            "cumulative project should stage in the product bridge");
    std::array<float, 64> output{};
    require(
        renderBridge(bridge, output) && hasAudibleSample(output)
            && bridge.latestPreparedScoreSchedule() != nullptr
            && bridge.latestPreparedLiveSet() != nullptr
            && bridge.latestPreparedLiveTools() != nullptr,
        "first audio boundary should adopt graph, Score, Live Set, and bounded Tool state together");

    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Osc,
        11,
        17);
    const auto staleRequest = bridge.requestLiveSetActivation(
        "main",
        "stop",
        talas::runtime::RealtimeLiveSetSourceKind::Osc,
        11,
        17);
    require(staleRequest.ok(),
            "matching loopback-style OSC authority should admit the mapped Stop request");
    bridge.publishLiveSetSourceAuthority(
        talas::runtime::RealtimeLiveSetSourceKind::Osc,
        11,
        19);
    (void) renderBridge(bridge, output);
    auto liveSetTelemetry = bridge.realtimeLiveSetTelemetry();
    require(
        liveSetTelemetry.completedActivations == 0
            && liveSetTelemetry.staleAuthority >= 1,
        "route replacement must revoke the queued prior-generation OSC request before side effects");
    const auto freshRequest = bridge.requestLiveSetActivation(
        "main",
        "stop",
        talas::runtime::RealtimeLiveSetSourceKind::Osc,
        11,
        19);
    require(freshRequest.ok(),
            "fresh route generation should remain admissible");
    (void) renderBridge(bridge, output);
    liveSetTelemetry = bridge.realtimeLiveSetTelemetry();
    require(liveSetTelemetry.completedActivations == 1,
            "fresh exact mapped Stop should execute once");

    bridge.setPlaying(true);
    auto replacement = document;
    replacement.soundDefinitions[1].graph.nodes.front().parameters[0] =
        expression("frequency", "440 hz");
    const auto priorStage = bridge.activeStageId();
    require(
        bridge.compileAndStageDocument(replacement, renderSettings()).ok()
            && renderBridge(bridge, output)
            && bridge.activeStageId() != 0
            && bridge.activeStageId() != priorStage
            && hasAudibleSample(output),
        "valid nested-Sound replacement should publish only at an audio boundary");
    const auto acceptedReplacementStage = bridge.activeStageId();

    const auto missingCandidate = missingComponentReplacement(replacement);
    const auto missingExecutionDiagnostics =
        talas::runtime::projectComponentExecutionDiagnostics(
            missingCandidate,
            registry);
    require(
        !missingExecutionDiagnostics.empty(),
        "missing-component fixture must be unavailable to execution before staging");
    const auto rejectedMissing = bridge.compileAndStageDocument(
        missingCandidate,
        renderSettings());
    require(
        !rejectedMissing.ok(),
        "missing-component replacement must fail before publication"
            + std::string{" (outcome "}
            + std::to_string(static_cast<int>(rejectedMissing.outcome))
            + ")"
            + (rejectedMissing.message.empty()
                   ? std::string{}
                   : ": " + rejectedMissing.message)
            + (rejectedMissing.diagnostics.empty()
                   ? std::string{}
                   : " | "
                       + rejectedMissing.diagnostics.front().message));
    require(
        bridge.activeStageId() == acceptedReplacementStage,
        "failed missing-component replacement must not change the active stage");
    require(
        renderBridge(bridge, output),
        "failed missing-component replacement must leave the prior graph rendering");
    require(
        bridge.activeStageId() == acceptedReplacementStage,
        "failed missing-component replacement must preserve the exact stage after the next audio boundary");
    require(
        hasAudibleSample(output),
        "failed missing-component replacement must preserve audible last-good output");

    require(bridge.requestTransportRelocation(128),
            "internal seek should publish for the accepted stage");
    require(
        renderBridge(bridge, output)
            && bridge.transportFramePosition() == 192,
        "seek should adopt at one audio boundary before advancing the block");
    bridge.stopTransport();
    require(
        !renderBridge(bridge, output)
            && bridge.transportFramePosition() == 0,
        "transport Stop should reset at the next silent callback boundary");

    bridge.clearAfterAudioStopped();
    require(
        bridge.activeStageId() == 0,
        "device stop should synchronously revoke the active graph stage");
    require(
        bridge.latestPreparedScoreSchedule() == nullptr,
        "device stop should synchronously revoke prepared Score authority");
    require(
        bridge.latestPreparedLiveSet() == nullptr,
        "device stop should synchronously revoke prepared Live Set authority");
    require(
        bridge.latestPreparedLiveTools() == nullptr,
        "device stop should synchronously revoke prepared Tool authority");
    auto restartedSettings = renderSettings(44100.0);
    bridge.setPlaying(true);
    require(
        bridge.compileAndStageDocument(replacement, restartedSettings).ok()
            && renderBridge(bridge, output)
            && bridge.activeStageId() != 0
            && bridge.latestPreparedScoreSchedule() != nullptr
            && bridge.latestPreparedLiveSet() != nullptr
            && bridge.latestPreparedLiveTools() != nullptr
            && hasAudibleSample(output),
        "device restart should reprepare and atomically republish the complete product authority at the new rate");
}
}

int main()
{
    testCurrentProjectAndWorkspaceRemainSeparateAndRecoverable();
    testOfflineRealtimeReplacementTransportDeviceAndIngressAuthority();
}
