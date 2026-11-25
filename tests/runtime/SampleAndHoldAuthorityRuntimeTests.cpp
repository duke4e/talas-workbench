#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
} // namespace

void *operator new(std::size_t size) {
  if (countAllocations.load(std::memory_order_relaxed))
    allocationCount.fetch_add(1, std::memory_order_relaxed);
  if (auto *memory = std::malloc(size))
    return memory;
  throw std::bad_alloc{};
}

void *operator new[](std::size_t size) {
  if (countAllocations.load(std::memory_order_relaxed))
    allocationCount.fetch_add(1, std::memory_order_relaxed);
  if (auto *memory = std::malloc(size))
    return memory;
  throw std::bad_alloc{};
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression booleanParameter(std::string id,
                                                         bool value) {
  talas::project::ProjectParameterExpression parameter;
  parameter.parameterId = std::move(id);
  parameter.boolean = value;
  return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id,
                                                      std::string value) {
  talas::project::ProjectParameterExpression parameter;
  parameter.parameterId = std::move(id);
  parameter.text = std::move(value);
  return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(std::string id,
                                                          std::string value) {
  talas::project::ProjectParameterExpression parameter;
  parameter.parameterId = std::move(id);
  parameter.resource = std::move(value);
  return parameter;
}

talas::project::ProjectNode soundInstance(std::string id,
                                       std::string definitionId) {
  talas::project::ProjectNode node;
  node.instanceId = std::move(id);
  node.soundDefinitionId = std::move(definitionId);
  return node;
}

talas::project::ProjectDocument makeDocument() {
  talas::project::ProjectDocument document;
  document.graph.nodes = {
      {"source", "talas.constant", {{"value", "!C185Value"}}},
      {"hold",
       "talas.sampleAndHold",
       {
           {"holdTime", "!C185Hold * 1 s"},
           {"offsetTime", "0.125 s"},
       }},
      {"carrier",
       "talas.oscillator",
       {
           {"frequency", "1 hz"},
           {"amplitude", "1"},
       }},
      {"vca", "talas.vca", {}},
      {"out", "talas.audioOutput", {}},
  };
  document.graph.connections = {
      {"source", "value", "hold", "value"},
      {"carrier", "audioOut", "vca", "audioIn"},
      {"hold", "value", "vca", "gain"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::runtime::RenderSettings makeSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C185Value", 0.25, talas::expr::ValueDomain::Scalar},
      {"C185Hold", 0.25, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {2, "C185Value", 0.5, talas::expr::ValueDomain::Scalar},
      {4, "C185Hold", 0.5, talas::expr::ValueDomain::Scalar},
      {6, "C185Value", 0.75, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::project::ProjectDocument makeConnectedDocument() {
  auto document = makeDocument();
  auto hold = std::ranges::find(document.graph.nodes, std::string_view{"hold"},
                                &talas::project::ProjectNode::instanceId);
  require(hold != document.graph.nodes.end(),
          "connected Sample And Hold fixture should contain its node");
  hold->parameters[0].expression = "1e200 * 1e200 * 1 s";
  document.graph.nodes.insert(
      document.graph.nodes.begin(),
      {"holdControl", "talas.constant", {{"value", "!C185ConnectedHold"}}});
  document.graph.connections.insert(
      document.graph.connections.begin(),
      {"holdControl", "value", "hold", "holdTime"});
  return document;
}

talas::runtime::RenderSettings makeConnectedSettings() {
  auto settings = makeSettings();
  settings.eventValues.push_back(
      {"C185ConnectedHold", 0.25, talas::expr::ValueDomain::Scalar});
  settings.eventValueChanges = {
      {2, "C185Value", 0.5, talas::expr::ValueDomain::Scalar},
      {4, "C185Hold", 0.5, talas::expr::ValueDomain::Scalar},
      {4, "C185ConnectedHold", 0.5, talas::expr::ValueDomain::Scalar},
      {6, "C185Value", 0.75, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

const talas::runtime::CompileStep &
holdStep(const talas::runtime::PreparedRuntimeGraph &graph) {
  const auto found =
      std::ranges::find(graph.plan().steps, std::string_view{"talas.sampleAndHold"},
                        &talas::runtime::CompileStep::typeId);
  require(found != graph.plan().steps.end(),
          "Sample And Hold authority fixture should contain its step");
  return *found;
}

talas::runtime::CompileStep &holdStep(talas::runtime::RuntimePlan &plan) {
  const auto found =
      std::ranges::find(plan.steps, std::string_view{"talas.sampleAndHold"},
                        &talas::runtime::CompileStep::typeId);
  require(found != plan.steps.end(),
          "Sample And Hold authority forgery should contain its step");
  return *found;
}

talas::runtime::BufferSpec &holdOutput(talas::runtime::RuntimePlan &plan) {
  const auto found = std::ranges::find_if(plan.buffers, [](const auto &buffer) {
    return buffer.nodeId == "hold" && buffer.portId == "value" &&
           buffer.type == talas::core::PortType::Control;
  });
  require(found != plan.buffers.end(),
          "Sample And Hold authority forgery should contain its output");
  return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.sampleAndHold");
  const std::vector<std::string> limitations{
      "offline-and-prepared-matched-checked-finite-periodic-sample-and-hold",
      "required-control-input-static-direct-event-value-or-per-frame-"
      "connected-positive-renderable-hold-time-and-static-non-negative-"
      "renderable-offset",
      "exact-nearest-frame-offset-live-per-frame-modulo-period-capture-hold-"
      "and-absolute-clock-lifecycle",
      "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-"
      "binding-buffer-producer-scope-zero-latency-and-local-transaction-"
      "authority",
      "allocation-free-prepared-callback-failure-atomic-clock-held-state-"
      "schedule-output-retry-and-reset",
      "proprietary-clock-reset-sync-phase-tempo-typed-event-interpolation-"
      "random-multichannel-and-exact-reference-semantics-unsupported",
  };
  require(descriptor != nullptr &&
              descriptor->capabilities.offline ==
                  talas::core::ExecutionSupport::Supported &&
              descriptor->capabilities.offlineBinding ==
                  talas::core::ExecutionBinding::LegacyAdapter &&
              descriptor->capabilities.preparedRealtime ==
                  talas::core::ExecutionSupport::Supported &&
              descriptor->capabilities.preparedRealtimeBinding ==
                  talas::core::ExecutionBinding::LegacyAdapter &&
              descriptor->capabilities.state ==
                  talas::core::NodeStateModel::SessionStateful &&
              descriptor->capabilities.supportsReset &&
              descriptor->capabilities.latency.kind ==
                  talas::core::LatencyKind::None &&
              descriptor->capabilities.fanInLatencyPolicy ==
                  talas::core::FanInLatencyPolicy::Align &&
              descriptor->capabilities.resourceAccess ==
                  talas::core::ResourceAccessMode::None &&
              descriptor->capabilities.limitations == limitations,
          "exact Sample And Hold descriptor should publish Supported authority");

  const auto compiled = talas::runtime::compileProject(
      makeDocument(), registry, makeSettings());
  require(compiled.ok() &&
              holdStep(*compiled.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              holdStep(*compiled.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "compiler should retain exact Sample And Hold Supported authority");

  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  std::array<double, 8> actual{};
  require(offline.ok() && prepared.ready(),
          "exact Sample And Hold should prepare in both renderers");
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto first = prepared.render(std::span<double>{actual}.first(3));
  const auto second =
      prepared.render(std::span<double>{actual}.subspan(3, 3));
  const auto third = prepared.render(std::span<double>{actual}.subspan(6));
  countAllocations.store(false, std::memory_order_relaxed);
  require(first && second && third &&
              allocationCount.load(std::memory_order_relaxed) == 0 &&
              std::ranges::equal(actual, offline.mono),
          "exact Sample And Hold should preserve irregular offline/prepared "
          "parity without callback allocation");

  talas::runtime::RealtimeRenderSession resetCandidate{*compiled.graph};
  talas::runtime::RealtimeRenderSession pristine{*compiled.graph};
  std::array<double, 4> scratch{};
  require(resetCandidate.ready() && pristine.ready() &&
              resetCandidate.render(scratch),
          "Sample And Hold reset fixture should advance state");
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  resetCandidate.resetTransport();
  countAllocations.store(false, std::memory_order_relaxed);
  std::array<double, 4> resetOutput{};
  std::array<double, 4> pristineOutput{};
  require(allocationCount.load(std::memory_order_relaxed) == 0 &&
              resetCandidate.render(resetOutput) &&
              pristine.render(pristineOutput) &&
              resetOutput == pristineOutput,
          "Sample And Hold reset should be allocation-free and restore clock, "
          "held state, and schedule cursors");

  talas::runtime::PreparedRuntimeGraph publicCopy{
      compiled.graph->plan(), compiled.graph->renderSettings()};
  talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
  require(!publicPrepared.ready() &&
              !talas::runtime::renderOffline(publicCopy, 4).ok(),
          "public graph reconstruction must not mint Supported Sample And Hold authority");
}

void testConnectedAuthorityAndForgeryRejection() {
  const auto connected = talas::runtime::compileProject(
      makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
      makeConnectedSettings());
  require(connected.ok(),
          "connected Sample And Hold fixture should compile: " +
              (connected.diagnostics.empty()
                   ? std::string{"missing diagnostic"}
                   : connected.diagnostics.front().message));
  require(holdStep(*connected.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              holdStep(*connected.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "connected hold time should own its non-finite fallback and retain "
          "Supported authority");
  const auto offline = talas::runtime::renderOffline(*connected.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*connected.graph};
  std::array<double, 8> actual{};
  require(offline.ok() && prepared.ready() &&
              prepared.render(std::span<double>{actual}.first(3)) &&
              prepared.render(std::span<double>{actual}.subspan(3, 3)) &&
              prepared.render(std::span<double>{actual}.subspan(6)) &&
              std::ranges::equal(actual, offline.mono),
          "connected hold time should preserve irregular partition parity");

  auto inertPayloadGraph = *connected.graph;
  auto &inertPayloadPlan = const_cast<talas::runtime::RuntimePlan &>(
      inertPayloadGraph.plan());
  holdStep(inertPayloadPlan).parameters[0].value = 185.0;
  const auto inertOffline = talas::runtime::renderOffline(inertPayloadGraph, 8);
  talas::runtime::RealtimeRenderSession inertPrepared{inertPayloadGraph};
  std::array<double, 8> inertActual{};
  require(inertOffline.ok() && inertPrepared.ready() &&
              inertPrepared.render(std::span<double>{inertActual}.first(4)) &&
              inertPrepared.render(std::span<double>{inertActual}.subspan(4)) &&
              std::ranges::equal(inertActual, inertOffline.mono) &&
              std::ranges::equal(inertActual, offline.mono),
          "connected hold time should make only its numeric fallback payload "
          "inert across both plan comparators");

  const auto requireForgeryRejected = [&connected](std::string_view label,
                                                    const auto &mutate) {
    auto forged = *connected.graph;
    auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
    mutate(plan);
    talas::runtime::RealtimeRenderSession candidate{forged};
    require(!candidate.ready() && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged connected " + std::string{label} +
                " Sample And Hold authority must fail in both renderers");
  };
  requireForgeryRejected("producer", [](auto &plan) {
    holdStep(plan).inputs[1].sourceNodeId = "C185ForgedProducer";
  });
  requireForgeryRejected("source buffer", [](auto &plan) {
    const auto bufferIndex = holdStep(plan).inputs[1].bufferIndex;
    require(bufferIndex < plan.buffers.size(),
            "connected hold-time input should own a source buffer");
    plan.buffers[bufferIndex].frames -= 1;
  });
}

void testDynamicFallbackIsHonestSubset() {
  auto document = makeDocument();
  document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VariablesV1;
  document.variableLibrary.variables.push_back({
      "hold", "Hold", talas::expr::ValueDomain::Scalar, 0.25,
      {talas::project::ProjectVariableOwnerKind::Project, {}}, true,
  });
  auto hold = std::ranges::find(document.graph.nodes, std::string_view{"hold"},
                                &talas::project::ProjectNode::instanceId);
  require(hold != document.graph.nodes.end(),
          "dynamic Sample And Hold fixture should contain its node");
  hold->parameters[0].expression = "@hold * 1 s";
  const auto dynamicHold = talas::runtime::compileProject(
      document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(dynamicHold.ok() &&
              holdStep(*dynamicHold.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              holdStep(*dynamicHold.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "variable-backed hold time must honestly downgrade both axes");

  auto dynamicOffsetDocument = makeDocument();
  auto dynamicOffset = std::ranges::find(
      dynamicOffsetDocument.graph.nodes, std::string_view{"hold"},
      &talas::project::ProjectNode::instanceId);
  require(dynamicOffset != dynamicOffsetDocument.graph.nodes.end(),
          "event-backed offset fixture should contain its node");
  dynamicOffset->parameters[1].expression = "!C185Offset * 1 s";
  auto offsetSettings = makeSettings();
  offsetSettings.eventValues.push_back(
      {"C185Offset", 0.125, talas::expr::ValueDomain::Scalar});
  const auto offset = talas::runtime::compileProject(
      dynamicOffsetDocument, talas::core::makeBuiltinNodeRegistry(),
      offsetSettings);
  require(offset.ok() &&
              holdStep(*offset.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              holdStep(*offset.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "event-backed offset must honestly downgrade both axes");

  const auto requireDynamicHoldDemotion = [](std::string expression,
                                             std::string_view label) {
    auto candidateDocument = makeDocument();
    candidateDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    auto candidate = std::ranges::find(
        candidateDocument.graph.nodes, std::string_view{"hold"},
        &talas::project::ProjectNode::instanceId);
    require(candidate != candidateDocument.graph.nodes.end(),
            "dynamic hold fixture should contain its node");
    candidate->parameters[0].expression = std::move(expression);
    const auto result = talas::runtime::compileProject(
        candidateDocument, talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(result.ok() &&
                holdStep(*result.graph).offlineSupport ==
                    talas::core::ExecutionSupport::Subset &&
                holdStep(*result.graph).preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Subset,
            std::string{label} +
                " hold time must honestly downgrade both axes");
  };
  requireDynamicHoldDemotion("previous(0.25, 0.25) * 1 s", "stateful");
  requireDynamicHoldDemotion("[pi, 0.25][1] * 1 s", "identifier-bearing");
}

void testNestedAndMidiVoiceScopeAuthority() {
  talas::project::ProjectSoundDefinition nestedDefinition;
  nestedDefinition.definitionId = "C185NestedSampleAndHold";
  nestedDefinition.displayName = "C185 Nested Sample And Hold";
  nestedDefinition.outputs.push_back(
      {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
  nestedDefinition.graph.nodes = {
      {"source", "talas.constant", {{"value", "0.5"}}},
      {"hold", "talas.sampleAndHold", {{"holdTime", "0.25 s"},
                                      {"offsetTime", "0.125 s"}}},
      {"carrier", "talas.oscillator", {{"frequency", "1 hz"},
                                      {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
  };
  nestedDefinition.graph.connections = {
      {"source", "value", "hold", "value"},
      {"hold", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
  };
  talas::project::ProjectDocument nestedDocument;
  nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
  nestedDocument.graph.nodes = {
      soundInstance("nested", "C185NestedSampleAndHold"),
      {"out", "talas.audioOutput", {}},
  };
  nestedDocument.graph.connections = {
      {"nested", "audioOut", "out", "audioIn"},
  };
  const talas::runtime::RenderSettings nestedSettings{8.0, 8};
  const auto nested = talas::runtime::compileProject(
      nestedDocument, talas::core::makeBuiltinNodeRegistry(), nestedSettings);
  require(nested.ok(), "nested Sample And Hold authority graph should compile");
  const auto &nestedHold = holdStep(*nested.graph);
  require(nestedHold.scopeId != talas::runtime::globalRuntimeScope &&
              nestedHold.eventValueScopeId == nestedHold.scopeId &&
              nestedHold.offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              nestedHold.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "nested Sample And Hold should retain Supported authority in its "
          "compiler-owned structural scope");
  const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
  talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
  std::array<double, 8> nestedActual{};
  require(nestedOffline.ok() && nestedPrepared.ready() &&
              nestedPrepared.render(nestedActual) &&
              std::ranges::equal(nestedActual, nestedOffline.mono),
          "nested Sample And Hold should preserve offline/prepared parity");

  auto forgedNested = *nested.graph;
  auto &forgedNestedPlan =
      const_cast<talas::runtime::RuntimePlan &>(forgedNested.plan());
  holdStep(forgedNestedPlan).scopeId = talas::runtime::globalRuntimeScope;
  talas::runtime::RealtimeRenderSession forgedNestedPrepared{forgedNested};
  require(!forgedNestedPrepared.ready() &&
              !talas::runtime::renderOffline(forgedNested, 4).ok(),
          "forged nested Sample And Hold topology must fail in both renderers");

  talas::project::ProjectDocument midiDocument;
  midiDocument.graph.nodes = {
      {"source", "talas.constant", {{"value", "0.5"}}},
      {"hold", "talas.sampleAndHold", {{"holdTime", "0.25 s"},
                                      {"offsetTime", "0 s"}}},
      {"carrier", "talas.oscillator", {{"frequency", "440 hz"},
                                      {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
      {"midi", "talas.midiVoice",
       {{"left", "1"},
        {"right", "1"},
        {"channel", "1"},
        textParameter("source", ""),
        resourceParameter("midiFile", ""),
        {"polyphony", "2"},
        {"lowPitch", "0 nn"},
        {"highPitch", "127 nn"},
        textParameter("script", ""),
        booleanParameter("shared", false)}},
      {"out", "talas.audioOutput", {}},
  };
  midiDocument.graph.connections = {
      {"source", "value", "hold", "value"},
      {"hold", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
      {"vca", "audioOut", "midi", "audioIn"},
      {"midi", "leftOut", "out", "audioIn"},
      {"midi", "rightOut", "out", "rightIn"},
  };
  talas::runtime::RenderSettings midiSettings{48000.0, 16};
  midiSettings.eventValues = {
      {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
      {"KeyVelocity", 0.0, talas::expr::ValueDomain::Scalar},
      {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
  };
  const auto midi = talas::runtime::compileProject(
      midiDocument, talas::core::makeBuiltinNodeRegistry(), midiSettings);
  require(midi.ok() && midi.graph->plan().midiVoices.size() == 1 &&
              midi.graph->plan().midiVoices.front().voices.size() == 2,
          "MIDI Voice Sample And Hold authority graph should compile two voices");
  const auto &midiRegion = midi.graph->plan().midiVoices.front();
  for (const auto &voice : midiRegion.voices) {
    const auto first = midi.graph->plan().steps.begin() +
                       static_cast<std::ptrdiff_t>(voice.firstStepIndex);
    const auto last = first + static_cast<std::ptrdiff_t>(voice.stepCount);
    const auto hold = std::ranges::find(
        first, last, std::string_view{"talas.sampleAndHold"},
        &talas::runtime::CompileStep::typeId);
    require(hold != last && hold->scopeId == voice.scopeId &&
                hold->structuralScopeId == talas::runtime::globalRuntimeScope &&
                hold->eventValueScopeId == hold->structuralScopeId &&
                hold->eventValueScopeId != hold->scopeId &&
                hold->offlineSupport == talas::core::ExecutionSupport::Supported &&
                hold->preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain exact Supported Sample And "
            "Hold authority in its compiler-owned voice scope");
  }
  const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
  talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
  std::array<double, 16> midiActual{};
  require(midiOffline.ok() && midiPrepared.ready() &&
              midiPrepared.render(midiActual) &&
              std::ranges::equal(midiActual, midiOffline.mono),
          "MIDI Voice Sample And Hold clones should preserve parity");
}

void testCustomCompatibilityAndAuthorityRejection() {
  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport(
      "talas.sampleAndHold", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.sampleAndHold", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  const auto subset = talas::runtime::compileProject(
      makeDocument(), std::move(subsetRegistry), makeSettings());
  talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
  require(subset.ok() && subsetPrepared.ready() &&
              talas::runtime::renderOffline(*subset.graph, 4).ok(),
          "custom both-Subset Sample And Hold should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.sampleAndHold", talas::core::FanInLatencyPolicy::NotApplicable);
  const auto divergent = talas::runtime::compileProject(
      makeDocument(), std::move(divergentRegistry), makeSettings());
  talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
  require(divergent.ok() && !divergentPrepared.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
          "divergent descriptor must not mint Supported Sample And Hold authority");

  const auto exact = talas::runtime::compileProject(
      makeDocument(), talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(exact.ok(), "exact Sample And Hold forgery fixture should compile");
  const auto requireForgeryRejected = [&exact](std::string_view label,
                                               const auto &mutate) {
    auto forged = *exact.graph;
    auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
    auto &settings =
        const_cast<talas::runtime::RenderSettings &>(forged.renderSettings());
    mutate(plan, settings);
    talas::runtime::RealtimeRenderSession prepared{forged};
    require(!prepared.ready() && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged " + std::string{label} +
                " Sample And Hold authority must fail in both renderers");
  };
  requireForgeryRejected("settings", [](auto &, auto &settings) {
    settings.maxBlockFrames += 1;
  });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C185Forged", {0.0}});
  });
  requireForgeryRejected("support", [](auto &plan, auto &) {
    holdStep(plan).preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    holdStep(plan).preparedRealtimeBinding = talas::core::ExecutionBinding::Kernel;
  });
  requireForgeryRejected("reset", [](auto &plan, auto &) {
    holdStep(plan).supportsReset = false;
  });
  requireForgeryRejected("scope", [](auto &plan, auto &) {
    holdStep(plan).scopeId = 185;
  });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    holdOutput(plan).nodeId = "C185ForgedOwner";
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    holdOutput(plan).frames -= 1;
  });
  requireForgeryRejected("duplicate output", [](auto &plan, auto &) {
    plan.buffers.push_back(holdOutput(plan));
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    holdStep(plan).resolvedLatencyFrames = 1;
  });
  requireForgeryRejected("event scope", [](auto &plan, auto &) {
    holdStep(plan).eventValueScopeId += 1;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    holdStep(plan).parameters.front().domain = talas::expr::ValueDomain::Scalar;
  });
}
} // namespace

int main() {
  testExactCapabilityAndPrivateCompilerAuthority();
  testConnectedAuthorityAndForgeryRejection();
  testDynamicFallbackIsHonestSubset();
  testNestedAndMidiVoiceScopeAuthority();
  testCustomCompatibilityAndAuthorityRejection();
  return 0;
}
