#include "talas/app/AnalysisWorkbenchModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditBoundary.h"
#include "talas/project/ProjectJson.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectDocument projectFixture()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"root-osc", "talas.oscillator", {}, {}});
    document.graph.nodes.push_back({"root-env", "talas.adsr", {}, {}});
    document.graph.nodes.push_back({
        "voice-a",
        "talas.sound",
        {},
        std::optional<std::string>{"voice"},
    });
    document.graph.nodes.push_back({
        "voice-b",
        "talas.sound",
        {},
        std::optional<std::string>{"voice"},
    });
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.graph.nodes.push_back({"nested-osc", "talas.oscillator", {}, {}});
    voice.graph.nodes.push_back({
        "layer-a",
        "talas.sound",
        {},
        std::optional<std::string>{"layer"},
    });
    talas::project::ProjectSoundDefinition layer;
    layer.definitionId = "layer";
    layer.displayName = "Layer";
    layer.graph.nodes.push_back({"deep-filter", "talas.filter", {}, {}});
    document.soundDefinitions = {voice, layer};
    return document;
}

talas::project::WorkspaceAnalysisTraceState trace(
    std::string traceId,
    std::vector<std::string> instancePath,
    std::optional<std::string> containingDefinitionId,
    std::string nodeId,
    talas::project::WorkspaceAnalysisTraceKind kind =
        talas::project::WorkspaceAnalysisTraceKind::Audio)
{
    talas::project::WorkspaceAnalysisTraceState result;
    result.traceId = std::move(traceId);
    result.instancePath = std::move(instancePath);
    result.containingDefinitionId = std::move(containingDefinitionId);
    result.nodeId = std::move(nodeId);
    result.portId = "audioOut";
    result.kind = kind;
    return result;
}

void testProjectionResolvesExactProjectAndNestedTargets()
{
    const auto document = projectFixture();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::WorkspaceAnalysisSessionState session;
    session.traces = {
        trace("root", {}, {}, "root-osc"),
        trace("voice", {"voice-a"}, "voice", "nested-osc"),
        trace("voice-b", {"voice-b"}, "voice", "nested-osc"),
        trace("deep", {"voice-a", "layer-a"}, "layer", "deep-filter"),
    };
    session.selectedTraceId = "voice";

    const auto projection = talas::app::buildAnalysisWorkbenchProjection(
        document, registry, session);
    require(projection.ok() && projection.traces.size() == 4,
        "valid root and nested Analysis targets should project");
    require(projection.selectedTraceIndex == std::optional<std::size_t>{1},
        "projection should preserve selected trace order");
    require(projection.traces[0].available()
            && projection.traces[0].scopeLabel == "Project",
        "root target should resolve only in the project graph");
    require(projection.traces[1].available()
            && projection.traces[1].scopeLabel == "voice-a",
        "one-level target should retain its exact instance identity");
    require(projection.traces[2].available()
            && projection.traces[2].scopeLabel == "voice-b",
        "the same definition node should resolve in a second exact instance");
    require(projection.traces[3].available()
            && projection.traces[3].scopeLabel == "voice-a / layer-a",
        "deep target should traverse the complete instance path");
    require(projection.traces[1].targetIdentity
                != projection.traces[2].targetIdentity
            && projection.traces[1].trace.nodeId
                == projection.traces[2].trace.nodeId,
        "the same definition node in two exact instances should never collapse to one target identity");
}

void testProjectionReportsExactMissingBoundary()
{
    const auto document = projectFixture();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                trace("missing-instance", {"ghost"}, "voice", "nested-osc"))
            == talas::app::AnalysisTraceAvailability::MissingInstance,
        "missing instance should not fall back to a matching definition node");
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                trace("wrong-definition", {"voice-a"}, "layer", "nested-osc"))
            == talas::app::AnalysisTraceAvailability::MissingDefinition,
        "mismatched containing definition should reject exact authority");
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                trace("missing-node", {"voice-a"}, "voice", "ghost"))
            == talas::app::AnalysisTraceAvailability::MissingNode,
        "missing nested node should remain explicitly unavailable");
    auto missingPort = trace("missing-port", {}, {}, "root-osc");
    missingPort.portId = "removed-output";
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                missingPort)
            == talas::app::AnalysisTraceAvailability::MissingPort,
        "removed prepared port should remain visibly unavailable");
    auto wrongKind = trace(
        "wrong-kind",
        {},
        {},
        "root-osc",
        talas::project::WorkspaceAnalysisTraceKind::Control);
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                wrongKind)
            == talas::app::AnalysisTraceAvailability::IncompatiblePortKind,
        "changed prepared payload kind should fail closed before staging");
    auto typedEvents = trace(
        "typed-events",
        {},
        {},
        "root-env",
        talas::project::WorkspaceAnalysisTraceKind::Event);
    typedEvents.portId.reset();
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                typedEvents)
            == talas::app::AnalysisTraceAvailability::Available,
        "descriptor-declared typed Event endpoint should be an exact node target");
    typedEvents.nodeId = "root-osc";
    require(talas::app::analysisTraceAvailability(
                document,
                registry,
                typedEvents)
            == talas::app::AnalysisTraceAvailability::IncompatiblePortKind,
        "node without typed Event authority must reject the synthetic target");
}

void testSessionMutationsAreBoundedAndDoNotDirtyProject()
{
    const auto document = projectFixture();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto bytes = talas::project::serializeProjectDocument(document);
    const talas::project::ProjectEditBoundary boundary(document);
    talas::project::WorkspaceAnalysisSessionState session;

    auto root = trace("root", {}, {}, "root-osc");
    require(talas::app::addAnalysisTrace(document, registry, session, root)
            && session.selectedTraceId == std::optional<std::string>{"root"},
        "first valid trace should become selected");
    require(!talas::app::addAnalysisTrace(document, registry, session, root),
        "duplicate exact Analysis target should reject");
    auto nested = trace("voice", {"voice-a"}, "voice", "nested-osc");
    require(talas::app::addAnalysisTrace(document, registry, session, nested),
        "different exact nested target should be admitted");
    require(talas::app::moveAnalysisTrace(session, "voice", 0)
            && session.traces.front().traceId == "voice",
        "trace reorder should preserve the stable trace id");
    require(talas::app::setAnalysisTraceVisible(session, "voice", false)
            && !session.traces.front().visible,
        "trace visibility should update only the selected workspace record");
    require(talas::app::setAnalysisTraceVerticalScale(session, "voice", 2.0)
            && session.traces.front().verticalScale == 2.0
            && !talas::app::setAnalysisTraceVerticalScale(
                session, "voice", std::numeric_limits<double>::infinity())
            && session.traces.front().verticalScale == 2.0,
        "trace scale should accept only the closed workspace range");
    require(talas::app::setAnalysisRefreshRateHz(session, 60)
            && session.refreshRateHz == 60
            && !talas::app::setAnalysisRefreshRateHz(session, 61)
            && session.refreshRateHz == 60,
        "Analysis refresh should accept only the closed message-thread rate");
    require(talas::app::setAnalysisHistoryFrames(session, 600)
            && session.historyFrames == 600
            && !talas::app::setAnalysisHistoryFrames(session, 0)
            && session.historyFrames == 600,
        "Analysis history should accept only the closed display-frame window");
    require(talas::app::selectAnalysisTrace(session, "voice")
            && session.selectedTraceId == std::optional<std::string>{"voice"},
        "selection should address a stable trace id");
    require(talas::app::removeAnalysisTrace(session, "voice")
            && session.selectedTraceId == std::optional<std::string>{"root"},
        "removing the selection should choose the first surviving trace");
    require(talas::project::serializeProjectDocument(document) == bytes
            && !boundary.isDirty(document),
        "Analysis workspace mutations must not enter project bytes or history");
}

void testProjectionRejectsDuplicateAndInvalidSessionAuthority()
{
    auto document = projectFixture();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::WorkspaceAnalysisSessionState session;
    session.traces = {
        trace("same", {}, {}, "root-osc"),
        trace("same", {"voice-a"}, "voice", "nested-osc"),
    };
    session.selectedTraceId = "missing";
    const auto projection = talas::app::buildAnalysisWorkbenchProjection(
        document, registry, session);
    require(!projection.ok() && projection.traces.size() == 1,
        "duplicate trace ids and stale selection should fail closed");

    session = {};
    for (std::size_t index = 0;
         index < talas::project::maximumWorkspaceAnalysisTraces;
         ++index)
    {
        const auto nodeId = "capacity-" + std::to_string(index);
        document.graph.nodes.push_back(
            {nodeId, "talas.oscillator", {}, {}});
        auto item = trace(
            "trace-" + std::to_string(index), {}, {}, nodeId);
        require(talas::app::addAnalysisTrace(
                    document,
                    registry,
                    session,
                    std::move(item)),
            "closed Analysis trace capacity should admit every in-range trace");
    }
    auto overflow = trace("overflow", {}, {}, "root-osc");
    require(!talas::app::addAnalysisTrace(
                document,
                registry,
                session,
                std::move(overflow)),
        "Analysis trace capacity should reject one record beyond the limit");
}

void testPreparedOutputKindsAdmitOnlyExactAnalysisShapes()
{
    const auto port = [](
        talas::core::PortType type,
        talas::core::SpectralFrameFormat format) {
        auto result = talas::core::PortDescriptor{"out", type, false};
        result.signal.spectralFormat = format;
        return result;
    };
    require(talas::app::analysisTraceKindForPreparedOutput(
                port(talas::core::PortType::Audio,
                    talas::core::SpectralFrameFormat::NotApplicable))
            == talas::project::WorkspaceAnalysisTraceKind::Audio,
        "prepared Audio output should map to an Audio trace");
    require(talas::app::analysisTraceKindForPreparedOutput(
                port(talas::core::PortType::Control,
                    talas::core::SpectralFrameFormat::NotApplicable))
            == talas::project::WorkspaceAnalysisTraceKind::Control,
        "prepared Control output should map to a Control trace");
    require(!talas::app::analysisTraceKindForPreparedOutput(
                 port(talas::core::PortType::Event,
                     talas::core::SpectralFrameFormat::NotApplicable))
                 .has_value(),
        "unbound graph Event ports should not masquerade as a prepared trace");
    require(talas::app::analysisTraceKindForPreparedOutput(
                port(talas::core::PortType::Spectrum,
                    talas::core::SpectralFrameFormat::PartialSpectrumV1))
            == talas::project::WorkspaceAnalysisTraceKind::PartialSpectrum,
        "exact PartialSpectrumV1 output should map to a partial trace");
    require(talas::app::analysisTraceKindForPreparedOutput(
                port(talas::core::PortType::ComplexSpectrum,
                    talas::core::SpectralFrameFormat::ComplexSpectrumV1))
            == talas::project::WorkspaceAnalysisTraceKind::ComplexFrame,
        "exact ComplexSpectrumV1 output should map to a complex trace");
    require(!talas::app::analysisTraceKindForPreparedOutput(
                 port(talas::core::PortType::Spectrum,
                     talas::core::SpectralFrameFormat::LegacyPartialTriples))
                 .has_value()
            && !talas::app::analysisTraceKindForPreparedOutput(
                    port(talas::core::PortType::ComplexSpectrum,
                        talas::core::SpectralFrameFormat::VariableOrUnknown))
                    .has_value(),
        "legacy or unknown spectral shapes must not silently enter the typed Analysis workspace");
}

void testSequentialHistoryRetainsNewestBoundedWindow()
{
    std::array<double, 600> storage{};
    const std::array first{0.25};
    std::array<double, 128> second{};
    std::array<double, 128> third{};
    for (std::size_t index = 0; index < second.size(); ++index)
    {
        second[index] = static_cast<double>(index + 1);
        third[index] = static_cast<double>(index + 129);
    }
    auto count = talas::app::appendAnalysisSequentialHistory(
        storage, 0, first, 240);
    count = talas::app::appendAnalysisSequentialHistory(
        storage, count, second, 240);
    count = talas::app::appendAnalysisSequentialHistory(
        storage, count, third, 240);
    require(count == 240
            && storage.front() == 17.0
            && storage[count - 1] == 256.0,
        "sequential history should retain exactly the newest bounded scalar frames");

    const std::array<double, 1> afterShrink{300.0};
    count = talas::app::appendAnalysisSequentialHistory(
        storage, count, afterShrink, 3);
    require(count == 3
            && storage[0] == 255.0
            && storage[1] == 256.0
            && storage[2] == 300.0,
        "history shrink should retain the true newest stored tail");

    const std::array<double, 4> newest{9.0, 10.0, 11.0, 12.0};
    count = talas::app::appendAnalysisSequentialHistory(
        storage, count, newest, 3);
    require(count == 3
            && storage[0] == 10.0
            && storage[2] == 12.0,
        "history reduction should keep only the newest values without allocation");
}

void testRuntimeEvidenceDistinguishesBusyFromUnavailableAndRejected()
{
    talas::app::AnalysisTraceRuntimeEvidence evidence;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::TargetUnavailable,
        "missing project target should remain unavailable before runtime evidence");

    evidence.targetAvailability =
        talas::app::AnalysisTraceAvailability::Available;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::CaptureUnavailable,
        "missing prepared capture should not look like no-new-data");
    evidence.captureReady = true;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::PreparedProbeUnavailable,
        "missing exact probe should remain explicit");
    evidence.probeBound = true;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::WaitingForCurrentStage,
        "unbound capture should wait for current-stage evidence");
    evidence.bindingReady = true;
    evidence.acceptedRecord = true;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::Live,
        "accepted current-stage record should become live");
    evidence.acceptedRecord = false;
    evidence.retainedLastGood = true;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::RetainedNoNewData,
        "busy or no-new-data may retain the last accepted record");
    evidence.rejectedEvidence = true;
    require(talas::app::analysisTraceRuntimeState(evidence)
            == talas::app::AnalysisTraceRuntimeState::RejectedEvidence,
        "rejected evidence must override and invalidate retained display data");
}

void testNextNodeTargetSelectsTypedEventsBeforePreparedOutput()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* adsr = registry.findNodeType("talas.adsr");
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    require(adsr != nullptr && oscillator != nullptr,
        "next-target fixture should resolve built-in descriptors");

    talas::project::WorkspaceAnalysisSessionState session;
    auto eventTrace = trace(
        "env-events",
        {},
        {},
        "root-env",
        talas::project::WorkspaceAnalysisTraceKind::Audio);
    require(talas::app::selectNextAnalysisTargetForNode(
                *adsr,
                session,
                eventTrace)
            && eventTrace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Event
            && !eventTrace.portId.has_value(),
        "first pin on a typed-events node should select its exact node endpoint");
    session.traces.push_back(eventTrace);

    auto outputTrace = trace(
        "env-output",
        {},
        {},
        "root-env",
        talas::project::WorkspaceAnalysisTraceKind::Event);
    require(talas::app::selectNextAnalysisTargetForNode(
                *adsr,
                session,
                outputTrace)
            && outputTrace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Control
            && outputTrace.portId == std::optional<std::string>{"value"},
        "second pin on the same typed-events node should select its first prepared output");

    auto audioTrace = trace(
        "osc-output",
        {},
        {},
        "root-osc",
        talas::project::WorkspaceAnalysisTraceKind::Event);
    require(talas::app::selectNextAnalysisTargetForNode(
                *oscillator,
                session,
                audioTrace)
            && audioTrace.kind
                == talas::project::WorkspaceAnalysisTraceKind::Audio
            && audioTrace.portId
                == std::optional<std::string>{"audioOut"},
        "ordinary node pin should select its first exact prepared output");
}

void testTraceKindsHaveClosedColourIndependentCodes()
{
    using Kind = talas::project::WorkspaceAnalysisTraceKind;
    const std::array cases{
        std::pair{Kind::Audio, std::string_view{"A"}},
        std::pair{Kind::Control, std::string_view{"C"}},
        std::pair{Kind::Event, std::string_view{"E"}},
        std::pair{Kind::PartialSpectrum, std::string_view{"D"}},
        std::pair{Kind::ComplexFrame, std::string_view{"D"}},
    };
    for (const auto& [kind, code] : cases)
    {
        require(
            talas::app::analysisTraceKindCode(kind) == code,
            "every Analysis trace family should retain its exact colour-independent code");
    }
}
}

int main()
{
    testProjectionResolvesExactProjectAndNestedTargets();
    testProjectionReportsExactMissingBoundary();
    testSessionMutationsAreBoundedAndDoNotDirtyProject();
    testProjectionRejectsDuplicateAndInvalidSessionAuthority();
    testPreparedOutputKindsAdmitOnlyExactAnalysisShapes();
    testSequentialHistoryRetainsNewestBoundedWindow();
    testRuntimeEvidenceDistinguishesBusyFromUnavailableAndRejected();
    testNextNodeTargetSelectsTypedEventsBeforePreparedOutput();
    testTraceKindsHaveClosedColourIndependentCodes();
    return 0;
}
