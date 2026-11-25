#include "app/workbench/WorkbenchComponentInternal.h"

#include <numeric>

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runVirtualControlIngressWorkflowForSmoke(
    std::string_view eventName)
{
    if (auto error = runMidiIngressGenerationWorkflowForSmoke(eventName))
        return error;
    if (auto error = virtualControlSurface_->runMidiMappingWorkflowForSmoke(
            eventName, {2, 74, 64}))
        return error;
    if (auto error = virtualControlSurface_->setOscMappingForSmoke(
            eventName, std::string{"/smoke/pitch"}))
        return "OSC mapping could not be authored: " + *error;
    if (auto error = virtualControlSurface_->setOscEgressMappingForSmoke(
            eventName, std::string{"/feedback/smoke-pitch"}))
        return "OSC feedback mapping could not be authored: " + *error;

    const auto originalLiveSetLibrary =
        projectBridge_.editor().document().liveSetLibrary;
    talas::project::ProjectSoundDefinition smokeLiveSound;
    smokeLiveSound.definitionId = "smokeLiveSound";
    smokeLiveSound.displayName = "Smoke Live Sound";
    smokeLiveSound.graph.nodes.push_back({
        "tone", "talas.oscillator", {},
    });
    smokeLiveSound.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "tone",
        "audioOut",
    });
    const auto liveSoundEdit = projectBridge_.editor().addSoundDefinition(
        std::move(smokeLiveSound));
    if (!liveSoundEdit.ok())
        return "Live Grid active-scroll Sound fixture could not be authored: "
            + liveSoundEdit.message;
    auto liveSetLibrary = originalLiveSetLibrary;
    talas::project::ProjectLiveSet smokeLiveSet;
    smokeLiveSet.liveSetId = "smoke-live-set";
    smokeLiveSet.displayName = "Smoke Live Set";
    talas::project::ProjectLiveSetCell smokeCell;
    smokeCell.cellId = "smoke-stop";
    smokeCell.displayName = "Smoke Stop";
    smokeCell.target = talas::project::ProjectLiveSetStopTarget{};
    smokeCell.mappings = {
        talas::project::ProjectLiveSetWorkbenchMapping{},
        talas::project::ProjectLiveSetMidiProgramMapping{
            4, 17, "smoke-midi-input"},
        talas::project::ProjectLiveSetOscMapping{"/smoke/live-grid"},
    };
    smokeLiveSet.cells.push_back(std::move(smokeCell));
    talas::project::ProjectLiveSetCell sparseCell;
    sparseCell.cellId = "smoke-sparse-stop";
    sparseCell.displayName = "Smoke Sparse Stop";
    sparseCell.row = 0;
    sparseCell.column = 15;
    sparseCell.target = talas::project::ProjectLiveSetSoundTarget{
        "smokeLiveSound", std::nullopt};
    sparseCell.mappings = {
        talas::project::ProjectLiveSetWorkbenchMapping{},
    };
    smokeLiveSet.cells.push_back(std::move(sparseCell));
    liveSetLibrary.liveSets.push_back(std::move(smokeLiveSet));
    talas::project::ProjectLiveSet overlappingLiveSet;
    overlappingLiveSet.liveSetId = "smoke-overlap-set";
    overlappingLiveSet.displayName = "Smoke Overlap Set";
    talas::project::ProjectLiveSetCell overlappingCell;
    overlappingCell.cellId = "smoke-overlap-stop";
    overlappingCell.displayName = "Smoke Overlap Stop";
    overlappingCell.row = 0;
    overlappingCell.column = 0;
    overlappingCell.target = talas::project::ProjectLiveSetStopTarget{};
    overlappingCell.mappings = {
        talas::project::ProjectLiveSetWorkbenchMapping{},
    };
    overlappingLiveSet.cells.push_back(std::move(overlappingCell));
    liveSetLibrary.liveSets.push_back(std::move(overlappingLiveSet));
    talas::project::ProjectLiveTool smokeTool;
    smokeTool.toolId = "smoke-tool";
    smokeTool.displayName = "Smoke Tool";
    smokeTool.initialStateId = "ready";
    smokeTool.states = {{"ready", "Ready"}, {"armed", "Armed"}};
    smokeTool.variables.push_back({
        "enabled",
        "Enabled",
        talas::project::ProjectLiveToolVariableKind::Boolean,
        0.0,
        false,
        std::nullopt,
    });
    talas::project::ProjectLiveToolTransition armTool;
    armTool.transitionId = "arm";
    armTool.fromStateId = "ready";
    armTool.toStateId = "armed";
    armTool.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::Manual;
    armTool.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    smokeTool.transitions.push_back(std::move(armTool));
    talas::project::ProjectLiveToolTransition resetTool;
    resetTool.transitionId = "reset";
    resetTool.fromStateId = "armed";
    resetTool.toStateId = "ready";
    resetTool.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    resetTool.trigger.variableId = "enabled";
    resetTool.guardExpression = "@enabled";
    resetTool.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    smokeTool.transitions.push_back(std::move(resetTool));
    talas::project::ProjectLiveToolTransition midiTool;
    midiTool.transitionId = "midi-arm";
    midiTool.fromStateId = "ready";
    midiTool.toStateId = "armed";
    midiTool.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::MidiProgram;
    midiTool.trigger.midiProgram = {
        4, 18, "smoke-midi-input"};
    midiTool.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    smokeTool.transitions.push_back(std::move(midiTool));
    talas::project::ProjectLiveToolTransition oscTool;
    oscTool.transitionId = "osc-reset";
    oscTool.fromStateId = "armed";
    oscTool.toStateId = "ready";
    oscTool.trigger.kind =
        talas::project::ProjectLiveToolTriggerKind::Osc;
    oscTool.trigger.osc.address = "/smoke/live-tool";
    oscTool.commands.push_back({
        talas::project::ProjectLiveToolCommandKind::Stop,
    });
    smokeTool.transitions.push_back(std::move(oscTool));
    liveSetLibrary.tools.push_back(std::move(smokeTool));
    const auto liveSetEdit = projectBridge_.editor().replaceLiveSetLibrary(
        std::move(liveSetLibrary));
    if (!liveSetEdit.ok())
        return "Live Grid smoke fixture could not be authored: "
            + liveSetEdit.message;
    documentChanged(false);

    const auto availablePort = [] {
        juce::DatagramSocket reservation(false);
        if (!reservation.bindToPort(0, "127.0.0.1"))
            return 0;
        return reservation.getBoundPort();
    }();
    if (availablePort < talas::project::minimumOscIngressPort)
        return "OSC lifecycle smoke could not reserve an unprivileged loopback port";

    juce::DatagramSocket feedbackReceiver(false);
    if (!feedbackReceiver.bindToPort(0, "127.0.0.1"))
        return "OSC feedback smoke could not reserve a loopback receiver";
    const auto feedbackPort = feedbackReceiver.getBoundPort();
    if (feedbackPort < talas::project::minimumOscIngressPort
        || feedbackPort == availablePort)
    {
        return "OSC feedback smoke did not reserve a distinct unprivileged port";
    }
    (void)setOscIngressSettings({true, availablePort});
    (void)setOscEgressSettings({true, feedbackPort});

    talas::runtime::RenderSettings oscSmokeSettings{48'000.0, 64};
    if (!stageProject(std::move(oscSmokeSettings), "OSC lifecycle smoke"))
        return "OSC lifecycle smoke could not stage its exact Event Value graph";
    std::array<float, 64> oscSmokeOutput{};
    std::array<float*, 1> oscSmokeChannels{oscSmokeOutput.data()};
    projectBridge_.setPlaying(true);
    const auto oscSmokeAdopted = projectBridge_.render(
        {}, 1.0, oscSmokeChannels.data(), oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    if (!oscSmokeAdopted)
        return "OSC lifecycle smoke could not adopt its staged graph";

    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    workspaceSession_.performance.selectedLiveSetId = "smoke-live-set";
    workspaceSession_.performance.selectedLiveSetCellId = "smoke-stop";
    workspaceSession_.performance.selectedLiveToolId = "smoke-tool";
    performancePanel_->showLiveGridForSmoke();
    performancePanel_->refresh();
    if (!performancePanel_->liveGridWorkflowIsAccessibleForSmoke())
        return "Live Grid did not expose its stable accessible 8 x 16 surface";
    if (!performancePanel_->liveToolWorkflowIsAccessibleForSmoke())
        return "Live Grid did not expose its bounded accessible Live Tool page";
    auto acceptedLiveTools =
        projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers;
    if (!performancePanel_->triggerSelectedLiveToolForSmoke()
        || projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers
            != acceptedLiveTools + 1U)
    {
        return "Live Tool manual control did not use the generation-bound realtime ingress";
    }
    ++acceptedLiveTools;
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveToolStatePersistedForSmoke("armed", false))
        return "Live Tool manual transition did not publish and persist its exact callback state";
    const auto guardRejectedBefore =
        projectBridge_.realtimeLiveToolTelemetry().guardRejected;
    if (!performancePanel_->applyRejectedSelectedLiveToolVariableForSmoke()
        || projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers
            != acceptedLiveTools + 1U)
    {
        return "Live Tool guard-rejection smoke did not enter the shared typed realtime ingress";
    }
    ++acceptedLiveTools;
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    performancePanel_->refreshRuntimeState();
    if (projectBridge_.realtimeLiveToolTelemetry().guardRejected
                != guardRejectedBefore + 1U
        || !performancePanel_->liveToolStatePersistedForSmoke(
            "armed", false)
        || !performancePanel_->liveToolDiagnosticContainsForSmoke(
            "reasons no-match/invalid/stale/guard/command/init 0/0/0/1/0/0"))
    {
        return "guard-rejected Live Tool variable did not hold last-good state and publish its exact bounded reason";
    }
    if (!performancePanel_->applySelectedLiveToolVariableForSmoke()
        || projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers
            != acceptedLiveTools + 1U)
    {
        return "Live Tool variable control did not use the shared typed realtime ingress";
    }
    ++acceptedLiveTools;
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveToolStatePersistedForSmoke("ready", true))
        return "Live Tool variable transition did not preserve its exact session-local value";
    auto acceptedLiveGrid =
        projectBridge_.realtimeLiveSetTelemetry().acceptedRequests;
    if (!performancePanel_->activateFirstLiveGridCellForSmoke())
        return "Live Grid pointer control was not enabled";
    if (projectBridge_.realtimeLiveSetTelemetry().acceptedRequests
            != acceptedLiveGrid + 1U)
    {
        return "Live Grid pointer activation did not use the shared exact dispatcher";
    }
    ++acceptedLiveGrid;
    if (!performancePanel_
             ->navigateToSparseLiveGridCellWithNativeKeyForSmoke())
    {
        return "Live Grid native arrow navigation did not skip sparse cells or scroll them into view";
    }
    if (!performancePanel_
             ->activateSelectedLiveGridCellWithNativeKeyForSmoke()
        || projectBridge_.realtimeLiveSetTelemetry().acceptedRequests
            != acceptedLiveGrid + 1U)
    {
        return "Live Grid keyboard activation did not use the shared exact dispatcher";
    }
    ++acceptedLiveGrid;
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveGridActiveCellIsVisibleForSmoke())
        return "externally adopted Live Grid cell was not scrolled into the compact viewport";
    if (!performancePanel_->liveGridActiveCellSurvivesResizeForSmoke())
        return "Live Grid layout resize hid the already active cell";
    handleIncomingMidiMessageFromSource(
        1,
        midiInputSourceGeneration_.load(std::memory_order_acquire),
        juce::MidiMessage::programChange(4, 17));
    processPendingMidiControllers();
    if (projectBridge_.realtimeLiveSetTelemetry().acceptedRequests
        != acceptedLiveGrid + 1U)
    {
        return "Live Grid MIDI Program Change did not use the shared exact dispatcher";
    }
    ++acceptedLiveGrid;
    handleIncomingMidiMessageFromSource(
        1,
        midiInputSourceGeneration_.load(std::memory_order_acquire),
        juce::MidiMessage::programChange(4, 18));
    processPendingMidiControllers();
    if (projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers
        != acceptedLiveTools + 1U)
    {
        return "Live Tool MIDI Program Change did not use its generation-bound source authority";
    }
    ++acceptedLiveTools;
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveToolStatePersistedForSmoke("armed", true))
        return "Live Tool MIDI transition did not publish the exact retained local state";

    const auto active = virtualControlSurface_
                            ->oscMappingPresentationForSmoke(eventName);
    const auto feedbackActive = virtualControlSurface_
                                    ->oscEgressMappingPresentationForSmoke(
                                        eventName);
    if (!active.has_value()
        || active->find("OSC in /smoke/pitch") == std::string::npos
        || active->find("OSC out /feedback/smoke-pitch")
            == std::string::npos
        || active->find("route unavailable") != std::string::npos
        || !active->ends_with("/smoke/pitch|Clear")
        || !feedbackActive.has_value()
        || !feedbackActive->ends_with(
            "/feedback/smoke-pitch|Clear")
        || latestOscIngressTelemetry_.activePort != availablePort)
    {
        return "OSC mapping did not refresh into its active loopback lifecycle state: "
            + active.value_or("<missing>")
            + "; diagnostic=" + oscIngressDiagnostic_.toStdString()
            + "; state=" + std::to_string(static_cast<int>(
                latestOscIngressTelemetry_.state))
            + "; activePort=" + std::to_string(
                latestOscIngressTelemetry_.activePort)
            + "; routesAvailable=" + std::to_string(oscRoutesAvailable_)
            + "; stagedRoutes=" + std::to_string(stagedMidiRoutesValid_)
            + "; routeStage=" + std::to_string(oscRouteStageGeneration_)
            + "; latestStage=" + std::to_string(
                projectBridge_.latestStageGeneration());
    }

    const auto receiveFeedback = [&]()
        -> std::optional<talas::runtime::OscControlMessage> {
        if (feedbackReceiver.waitUntilReady(true, 1'000) <= 0)
            return std::nullopt;
        std::array<std::uint8_t,
                   talas::runtime::maximumOscControlMessageBytes>
            packet{};
        const auto packetSize = feedbackReceiver.read(
            packet.data(), static_cast<int>(packet.size()), false);
        std::array<talas::runtime::OscControlMessage, 1> messages{};
        const auto parsed = packetSize > 0
            ? talas::runtime::parseOscControlPacket(
                  std::span<const std::uint8_t>{
                      packet.data(), static_cast<std::size_t>(packetSize)},
                  messages)
            : talas::runtime::OscPacketParseResult{};
        return parsed.accepted() && parsed.messageCount == 1
            ? std::optional{messages.front()}
            : std::nullopt;
    };
    const auto initialFeedback = receiveFeedback();
    if (!initialFeedback.has_value()
        || initialFeedback->addressView() != "/feedback/smoke-pitch"
        || latestOscEgressTelemetry_.activePort != feedbackPort)
    {
        return "OSC feedback smoke did not publish one exact canonical resync";
    }

    juce::DatagramSocket ingressSender(false);
    std::array<std::uint8_t, talas::runtime::maximumOscControlMessageBytes>
        ingressPacket{};
    const auto encodedIngress = talas::runtime::encodeOscControlMessage(
        "/smoke/pitch", 0.375, ingressPacket);
    const auto acceptedBefore = oscIngress_.telemetry().acceptedMessages;
    if (!encodedIngress.encoded()
        || ingressSender.write(
               "127.0.0.1",
               availablePort,
               ingressPacket.data(),
               static_cast<int>(encodedIngress.packetSize))
            != static_cast<int>(encodedIngress.packetSize))
    {
        return "OSC feedback smoke could not send its ingress no-echo fixture";
    }
    const auto ingressAcceptedDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < ingressAcceptedDeadline
           && oscIngress_.telemetry().acceptedMessages == acceptedBefore)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (oscIngress_.telemetry().acceptedMessages != acceptedBefore + 1)
        return "OSC feedback no-echo fixture was not accepted by OSC ingress";
    if (feedbackReceiver.waitUntilReady(true, 100) > 0)
        return "accepted OSC ingress must not echo through OSC feedback";

    const auto encodedLiveGrid = talas::runtime::encodeOscControlMessage(
        "/smoke/live-grid", 1.0, ingressPacket);
    const auto liveGridOscAcceptedBefore =
        oscIngress_.telemetry().acceptedMessages;
    if (!encodedLiveGrid.encoded()
        || ingressSender.write(
               "127.0.0.1",
               availablePort,
               ingressPacket.data(),
               static_cast<int>(encodedLiveGrid.packetSize))
            != static_cast<int>(encodedLiveGrid.packetSize))
    {
        return "Live Grid OSC smoke could not send its exact mapped packet";
    }
    const auto liveGridOscDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < liveGridOscDeadline
           && oscIngress_.telemetry().acceptedMessages
                == liveGridOscAcceptedBefore)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    processPendingMidiControllers();
    if (oscIngress_.telemetry().acceptedMessages
            != liveGridOscAcceptedBefore + 1U
        || projectBridge_.realtimeLiveSetTelemetry().acceptedRequests
            != acceptedLiveGrid + 1U)
    {
        return "Live Grid OSC did not traverse the bounded listener queue and shared dispatcher";
    }
    ++acceptedLiveGrid;
    if (feedbackReceiver.waitUntilReady(true, 100) > 0)
        return "accepted Live Grid OSC must not echo through OSC feedback";
    const auto encodedLiveTool = talas::runtime::encodeOscControlMessage(
        "/smoke/live-tool", 1.0, ingressPacket);
    const auto liveToolOscAcceptedBefore =
        oscIngress_.telemetry().acceptedMessages;
    if (!encodedLiveTool.encoded()
        || ingressSender.write(
               "127.0.0.1",
               availablePort,
               ingressPacket.data(),
               static_cast<int>(encodedLiveTool.packetSize))
            != static_cast<int>(encodedLiveTool.packetSize))
    {
        return "Live Tool OSC smoke could not send its exact mapped packet";
    }
    const auto liveToolOscDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < liveToolOscDeadline
           && oscIngress_.telemetry().acceptedMessages
                == liveToolOscAcceptedBefore)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    processPendingMidiControllers();
    if (oscIngress_.telemetry().acceptedMessages
            != liveToolOscAcceptedBefore + 1U
        || projectBridge_.realtimeLiveToolTelemetry().acceptedTriggers
            != acceptedLiveTools + 1U)
    {
        return "Live Tool OSC did not traverse the bounded listener queue and exact tool dispatcher";
    }
    ++acceptedLiveTools;
    if (feedbackReceiver.waitUntilReady(true, 100) > 0)
        return "accepted Live Tool OSC must not echo through OSC feedback";
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    if (projectBridge_.realtimeLiveSetTelemetry().completedActivations == 0)
        return "Live Grid smoke did not adopt its newest queued Stop cell";
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveToolStatePersistedForSmoke("ready", true))
        return "Live Tool OSC transition did not preserve its exact session-local state";
    talas::runtime::RenderSettings restoreRaceSettings{48'000.0, 64};
    if (!stageProject(
            std::move(restoreRaceSettings),
            "Live Tool restore acknowledgment smoke"))
    {
        return "Live Tool restore acknowledgment smoke could not restage its exact project";
    }
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    performancePanel_->refreshRuntimeState();
    performancePanel_->refreshLiveToolRestoreInterleavingForSmoke([&] {
        (void)projectBridge_.render(
            {},
            juce::Time::getMillisecondCounterHiRes() * 0.001,
            oscSmokeChannels.data(),
            oscSmokeChannels.size(),
            oscSmokeOutput.size());
    });
    if (!performancePanel_->liveToolStatePersistedForSmoke("ready", true))
    {
        return "Live Tool restore acknowledgment persisted the pre-restore runtime snapshot";
    }
    if (!performancePanel_->triggerSelectedLiveToolForSmoke())
        return "Live Tool restore acknowledgment smoke could not queue a post-restore user transition";
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    projectBridge_.setPlaying(false);
    performancePanel_->refreshRuntimeState();
    if (!performancePanel_->liveToolStatePersistedForSmoke("armed", true))
    {
        return "Live Tool restore acknowledgment remained pending after callback adoption and hid a newer user transition";
    }
    if (workspaceSession_.performance.queuedLiveSetRequestGeneration != 0
        || workspaceSession_.performance.queuedLiveSetId.has_value()
        || workspaceSession_.performance.queuedLiveSetCellId.has_value())
    {
        return "Live Grid completed Stop remained queued in WorkspaceSession";
    }
    const auto restoreLiveSets =
        projectBridge_.editor().replaceLiveSetLibrary(originalLiveSetLibrary);
    if (!restoreLiveSets.ok())
        return "Live Grid smoke fixture could not restore the prior library: "
            + restoreLiveSets.message;
    const auto removeLiveSound =
        projectBridge_.editor().removeSoundDefinition("smokeLiveSound");
    if (!removeLiveSound.ok())
        return "Live Grid smoke fixture could not remove its temporary Sound: "
            + removeLiveSound.message;
    workspaceSession_.performance.selectedLiveSetId.reset();
    workspaceSession_.performance.selectedLiveSetCellId.reset();
    workspaceSession_.performance.activeLiveSetId.reset();
    workspaceSession_.performance.activeLiveSetCellId.reset();
    workspaceSession_.performance.selectedLiveToolId.reset();
    workspaceSession_.performance.liveToolStates.clear();
    documentChanged(false);
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    for (std::size_t pending = 0;
         pending < 8 && feedbackReceiver.waitUntilReady(true, 0) > 0;
         ++pending)
    {
        (void)receiveFeedback();
    }

    std::array<std::uint8_t, talas::runtime::maximumOscUdpPacketBytes>
        timedIngressPacket{};
    const auto encodedTimedIngress = talas::runtime::encodeOscControlMessage(
        "/smoke/pitch", 0.375, ingressPacket);
    if (!encodedTimedIngress.encoded())
        return "OSC NTP smoke could not encode its future loopback value";
    std::ranges::copy(
        std::array<std::uint8_t, 8>{
            '#', 'b', 'u', 'n', 'd', 'l', 'e', 0},
        timedIngressPacket.begin());
    const auto wallNanoseconds = std::chrono::duration_cast<
        std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                                     .count();
    const auto ntpAnchor = talas::runtime::makeOscNtpClockAnchor(
        wallNanoseconds, 1);
    const auto timedTag = ntpAnchor.timetag
        + talas::runtime::oscNtpFractionUnitsPerSecond / 5;
    const auto writeBig32 = [&](std::size_t offset, std::uint32_t value) {
        for (std::size_t byte = 0; byte < 4; ++byte)
        {
            timedIngressPacket[offset + byte] = static_cast<std::uint8_t>(
                value >> (24 - byte * 8));
        }
    };
    writeBig32(8, static_cast<std::uint32_t>(timedTag >> 32));
    writeBig32(12, static_cast<std::uint32_t>(timedTag));
    writeBig32(
        16, static_cast<std::uint32_t>(encodedTimedIngress.packetSize));
    std::ranges::copy(
        std::span<const std::uint8_t>{
            ingressPacket.data(), encodedTimedIngress.packetSize},
        timedIngressPacket.begin() + 20);
    const auto scheduledBefore =
        oscIngress_.telemetry().scheduledMessages;
    const auto appliedBefore = projectBridge_.realtimeControlTelemetry()
                                   .scheduledApplied;
    const auto timedPacketSize = 20 + encodedTimedIngress.packetSize;
    if (!ntpAnchor.valid()
        || ingressSender.write(
               "127.0.0.1",
               availablePort,
               timedIngressPacket.data(),
               static_cast<int>(timedPacketSize))
            != static_cast<int>(timedPacketSize))
    {
        return "OSC NTP smoke could not send its future loopback bundle";
    }
    const auto scheduledDeadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < scheduledDeadline
           && oscIngress_.telemetry().scheduledMessages
                == scheduledBefore)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    projectBridge_.setPlaying(true);
    (void)projectBridge_.render(
        {},
        juce::Time::getMillisecondCounterHiRes() * 0.001,
        oscSmokeChannels.data(),
        oscSmokeChannels.size(),
        oscSmokeOutput.size());
    oscLoopbackProfile_.refreshTelemetry();
    latestOscLoopbackTelemetry_ = oscLoopbackProfile_.telemetry();
    const auto pendingAccessible = oscProfileAccessibleDetail();
    while (std::chrono::steady_clock::now() < scheduledDeadline
           && projectBridge_.realtimeControlTelemetry().scheduledApplied
                == appliedBefore)
    {
        (void)projectBridge_.render(
            {},
            juce::Time::getMillisecondCounterHiRes() * 0.001,
            oscSmokeChannels.data(),
            oscSmokeChannels.size(),
            oscSmokeOutput.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    projectBridge_.setPlaying(false);
    if (oscIngress_.telemetry().scheduledMessages != scheduledBefore + 1
        || projectBridge_.realtimeControlTelemetry().scheduledApplied
            != appliedBefore + 1
        || !pendingAccessible.contains("NTP pending 1")
        || feedbackReceiver.waitUntilReady(true, 100) > 0)
    {
        return "OSC NTP smoke did not schedule, project, and apply one local-clock value without echo";
    }

    const auto& controls = projectBridge_.editor().document().virtualControls;
    const auto smokeControl = std::ranges::find(
        controls, eventName, &talas::project::VirtualControl::eventName);
    if (smokeControl == controls.end()
        || !virtualControlSurface_->setValueForSmoke(
            eventName,
            std::midpoint(smokeControl->minimum, smokeControl->maximum)))
    {
        return "OSC feedback smoke could not apply a persisted UI value";
    }
    const auto uiFeedback = receiveFeedback();
    if (!uiFeedback.has_value()
        || uiFeedback->addressView() != "/feedback/smoke-pitch"
        || std::abs(uiFeedback->normalizedValue - 0.5) > 1.0e-12)
    {
        return "OSC feedback smoke did not publish the persisted UI value: "
            + (uiFeedback.has_value()
                   ? std::string{uiFeedback->addressView()} + "="
                       + std::to_string(uiFeedback->normalizedValue)
                   : std::string{"<missing>"});
    }

    (void)virtualControlSurface_->applyMidiController(
        {2, 74, 64}, "smoke-midi-input");
    std::optional<talas::runtime::OscControlMessage> midiFeedback;
    for (std::size_t attempt = 0; attempt < 4; ++attempt)
    {
        const auto candidate = receiveFeedback();
        if (!candidate.has_value())
            break;
        midiFeedback = candidate;
        if (candidate->addressView() == "/feedback/smoke-pitch"
            && std::abs(
                candidate->normalizedValue - 64.0 / 127.0) <= 1.0e-12)
        {
            break;
        }
    }
    virtualControlSurface_->finishMidiTransactionNow();
    if (!midiFeedback.has_value()
        || midiFeedback->addressView() != "/feedback/smoke-pitch"
        || std::abs(midiFeedback->normalizedValue - 64.0 / 127.0)
            > 1.0e-12)
    {
        return "OSC feedback smoke did not publish the reconciled MIDI value: "
            + (midiFeedback.has_value()
                   ? std::string{midiFeedback->addressView()} + "="
                       + std::to_string(midiFeedback->normalizedValue)
                   : std::string{"<missing>"});
    }

    juce::DatagramSocket conflict(false);
    if (!conflict.bindToPort(0, "127.0.0.1"))
        return "OSC lifecycle smoke could not reserve its conflicting port";
    const auto failedPort = conflict.getBoundPort();
    (void)setOscIngressSettings({true, failedPort});
    const auto unavailable = virtualControlSurface_
                                 ->oscMappingPresentationForSmoke(eventName);
    if (!unavailable.has_value()
        || unavailable->find("OSC in unavailable: /smoke/pitch")
            == std::string::npos
        || latestOscIngressTelemetry_.activePort != availablePort
        || latestOscIngressTelemetry_.failedPort != failedPort
        || !oscIngressDiagnostic_.contains(
            "requested 127.0.0.1:" + juce::String(failedPort))
        || !oscIngressDiagnostic_.contains(
            "still listening on 127.0.0.1:"
            + juce::String(availablePort)))
    {
        return "OSC mapping did not refresh into its retained-port failure state: "
            + unavailable.value_or("<missing>");
    }
    (void)setOscIngressSettings({false, failedPort});
    (void)setOscEgressSettings({false, feedbackPort});
    if (auto error = virtualControlSurface_->setOscMappingForSmoke(
            eventName, std::nullopt))
        return "OSC mapping could not be cleared: " + *error;
    if (auto error = virtualControlSurface_->setOscEgressMappingForSmoke(
            eventName, std::nullopt))
        return "OSC feedback mapping could not be cleared: " + *error;
    const auto unmapped = virtualControlSurface_
                              ->oscMappingPresentationForSmoke(eventName);
    const auto feedbackUnmapped = virtualControlSurface_
                                      ->oscEgressMappingPresentationForSmoke(
                                          eventName);
    if (!unmapped.has_value()
        || unmapped->find("OSC in unmapped") == std::string::npos
        || unmapped->find("OSC out unmapped") == std::string::npos
        || !unmapped->ends_with("|Set")
        || !feedbackUnmapped.has_value()
        || !feedbackUnmapped->ends_with("|Set"))
        return "OSC unmap did not restore the explicit unmapped state";
    return std::nullopt;
}
}
