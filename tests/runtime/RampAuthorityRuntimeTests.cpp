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

talas::project::ProjectParameterExpression choiceParameter(std::string id,
                                                        std::string value) {
  talas::project::ProjectParameterExpression parameter;
  parameter.parameterId = std::move(id);
  parameter.choice = std::move(value);
  return parameter;
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
      {"ramp",
       "talas.ramp",
       {
           {"trigger", "!C184Trigger"},
           {"duration", "!C184Duration * 1 s"},
           choiceParameter("variant", "repeatingFullRamp"),
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
      {"carrier", "audioOut", "vca", "audioIn"},
      {"ramp", "value", "vca", "gain"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::runtime::RenderSettings makeSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C184Trigger", 0.0, talas::expr::ValueDomain::Scalar},
      {"C184Duration", 0.5, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {1, "C184Trigger", 1.0, talas::expr::ValueDomain::Scalar},
      {3, "C184Trigger", 0.0, talas::expr::ValueDomain::Scalar},
      {5, "C184Duration", 0.25, talas::expr::ValueDomain::Scalar},
      {5, "C184Trigger", 1.0, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::project::ProjectDocument makeConnectedDocument() {
  auto document = makeDocument();
  auto ramp = std::ranges::find(document.graph.nodes,
                                std::string_view{"ramp"},
                                &talas::project::ProjectNode::instanceId);
  require(ramp != document.graph.nodes.end(),
          "connected Ramp fixture should contain its node");
  ramp->parameters = {
      {"trigger", "0"},
      {"duration", "1 s"},
      choiceParameter("variant", "fullRamp"),
  };
  document.graph.nodes.insert(
      document.graph.nodes.begin(),
      {
          {"triggerControl",
           "talas.constant",
           {{"value", "!C184ConnectedTrigger"}}},
          {"durationControl",
           "talas.constant",
           {{"value", "!C184ConnectedDuration"}}},
      });
  document.graph.connections.insert(
      document.graph.connections.begin(),
      {
          {"triggerControl", "value", "ramp", "trigger"},
          {"durationControl", "value", "ramp", "duration"},
      });
  return document;
}

talas::runtime::RenderSettings makeConnectedSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C184ConnectedTrigger", 0.0, talas::expr::ValueDomain::Scalar},
      {"C184ConnectedDuration", 0.5, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {1, "C184ConnectedTrigger", 1.0, talas::expr::ValueDomain::Scalar},
      {3, "C184ConnectedTrigger", 0.0, talas::expr::ValueDomain::Scalar},
      {5, "C184ConnectedDuration", 0.25, talas::expr::ValueDomain::Scalar},
      {5, "C184ConnectedTrigger", 1.0, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::runtime::CompileResult
compile(talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry()) {
  return talas::runtime::compileProject(makeDocument(), std::move(registry),
                                     makeSettings());
}

const talas::runtime::CompileStep &
rampStep(const talas::runtime::PreparedRuntimeGraph &graph) {
  const auto found =
      std::ranges::find(graph.plan().steps, std::string_view{"talas.ramp"},
                        &talas::runtime::CompileStep::typeId);
  require(found != graph.plan().steps.end(),
          "Ramp authority fixture should contain its step");
  return *found;
}

talas::runtime::CompileStep &rampStep(talas::runtime::RuntimePlan &plan) {
  const auto found =
      std::ranges::find(plan.steps, std::string_view{"talas.ramp"},
                        &talas::runtime::CompileStep::typeId);
  require(found != plan.steps.end(),
          "Ramp authority forgery should contain its step");
  return *found;
}

talas::runtime::BufferSpec &rampOutput(talas::runtime::RuntimePlan &plan) {
  const auto found = std::ranges::find_if(plan.buffers, [](const auto &buffer) {
    return buffer.nodeId == "ramp" && buffer.portId == "value" &&
           buffer.type == talas::core::PortType::Control;
  });
  require(found != plan.buffers.end(),
          "Ramp authority forgery should contain its output");
  return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.ramp");
  const auto controlSignal =
      talas::core::defaultPortSignalContract(talas::core::PortType::Control);
  const auto exactControlPort =
      [&controlSignal](const talas::core::PortDescriptor &port,
                       std::string_view id) {
        return port.id == id && port.type == talas::core::PortType::Control &&
               !port.required && !port.compilerPopulated &&
               port.signal.rate == controlSignal.rate &&
               port.signal.audioLayout == controlSignal.audioLayout &&
               port.signal.edge == controlSignal.edge &&
               port.signal.timelineBearing == controlSignal.timelineBearing &&
               port.signal.spectralFormat == controlSignal.spectralFormat;
      };
  const auto exactExpression =
      [](const talas::core::ParameterDescriptor &parameter, std::string_view id,
         talas::expr::ValueDomain domain, std::string_view defaultExpression) {
        return parameter.id == id &&
               parameter.kind == talas::core::ParameterKind::Expression &&
               parameter.domain == domain &&
               parameter.defaultExpression == defaultExpression &&
               parameter.defaultChoice.empty() && parameter.choices.empty() &&
               !parameter.defaultBoolean.has_value() &&
               parameter.defaultResource.empty() &&
               parameter.defaultText.empty() &&
               parameter.resourceAuthority ==
                   talas::core::ParameterResourceAuthority::None;
      };
  const std::vector<std::string> limitations{
      "offline-and-prepared-matched-checked-finite-edge-triggered-ramp",
      "static-direct-event-value-or-per-frame-connected-selected-finite-"
      "trigger-and-positive-renderable-edge-captured-duration",
      "exact-four-variant-positive-edge-rearm-pre-increment-endpoint-hold-"
      "and-repeat-boundary-lifecycle",
      "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-"
      "binding-buffer-producer-scope-zero-latency-and-local-transaction-"
      "authority",
      "allocation-free-prepared-callback-failure-atomic-state-schedule-output-"
      "retry-and-reset",
      "proprietary-curve-zero-negative-duration-explicit-reset-sync-phase-"
      "tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported",
  };
  require(
      descriptor != nullptr && descriptor->displayName == "Ramp" &&
          descriptor->category == "Control" &&
          descriptor->supportLevel == talas::core::NodeSupportLevel::Utility &&
          descriptor->supportSummary ==
              "Talas utility." &&
          descriptor->inputs.size() == 2 &&
          exactControlPort(descriptor->inputs[0], "trigger") &&
          exactControlPort(descriptor->inputs[1], "duration") &&
          descriptor->outputs.size() == 1 &&
          exactControlPort(descriptor->outputs[0], "value") &&
          descriptor->parameters.size() == 3 &&
          exactExpression(descriptor->parameters[0], "trigger",
                          talas::expr::ValueDomain::Scalar, "1.0") &&
          exactExpression(descriptor->parameters[1], "duration",
                          talas::expr::ValueDomain::Seconds, "1 s") &&
          descriptor->parameters[2].id == "variant" &&
          descriptor->parameters[2].kind ==
              talas::core::ParameterKind::Choice &&
          descriptor->parameters[2].domain ==
              talas::expr::ValueDomain::Scalar &&
          descriptor->parameters[2].defaultChoice == "ramp" &&
          descriptor->parameters[2].choices ==
              std::vector<std::string>{"ramp", "fullRamp", "repeatingRamp",
                                       "repeatingFullRamp"} &&
          descriptor->parameters[2].defaultExpression.empty() &&
          !descriptor->parameters[2].defaultBoolean.has_value() &&
          descriptor->parameters[2].defaultResource.empty() &&
          descriptor->parameters[2].defaultText.empty() &&
          descriptor->parameters[2].resourceAuthority ==
              talas::core::ParameterResourceAuthority::None &&
          descriptor->capabilities.offline ==
              talas::core::ExecutionSupport::Supported &&
          descriptor->capabilities.offlineBinding ==
              talas::core::ExecutionBinding::LegacyAdapter &&
          descriptor->capabilities.preparedRealtime ==
              talas::core::ExecutionSupport::Supported &&
          descriptor->capabilities.preparedRealtimeBinding ==
              talas::core::ExecutionBinding::LegacyAdapter &&
          descriptor->capabilities.nestedSounds ==
              talas::core::NestedSoundSupport::Expanded &&
          descriptor->capabilities.state ==
              talas::core::NodeStateModel::SessionStateful &&
          descriptor->capabilities.supportsReset &&
          !descriptor->capabilities.typedEvents &&
          !descriptor->capabilities.transportAware &&
          descriptor->capabilities.latency.kind ==
              talas::core::LatencyKind::None &&
          descriptor->capabilities.latency.fixedFrames == 0 &&
          descriptor->capabilities.latency.parameterId.empty() &&
          descriptor->capabilities.latency.preparedRule ==
              talas::core::PreparedLatencyRule::None &&
          descriptor->capabilities.resourceAccess ==
              talas::core::ResourceAccessMode::None &&
          descriptor->capabilities.fanInLatencyPolicy ==
              talas::core::FanInLatencyPolicy::Align &&
          descriptor->capabilities.outputBusLayouts.empty() &&
          descriptor->capabilities.limitations == limitations &&
          descriptor->typeVersion == "1.0.0" &&
          descriptor->stateSchemaVersion == 1 &&
          descriptor->provenance.vendor == "Talas" &&
          descriptor->provenance.origin == talas::core::NodeOrigin::BuiltIn,
      "exact Ramp descriptor should publish complete Supported authority");

  const auto compiled = compile(registry);
  require(compiled.ok(), "exact Ramp authority graph should compile");
  const auto &step = rampStep(*compiled.graph);
  require(step.offlineSupport == talas::core::ExecutionSupport::Supported &&
              step.offlineBinding ==
                  talas::core::ExecutionBinding::LegacyAdapter &&
              step.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported &&
              step.preparedRealtimeBinding ==
                  talas::core::ExecutionBinding::LegacyAdapter &&
              step.supportsReset && !step.kernelKind.has_value() &&
              !step.preparedIsolatedComponent &&
              step.resolvedLatencyFrames == 0,
          "compiler should retain exact promoted Ramp plan authority");

  talas::runtime::PreparedRuntimeGraph publicCopy{
      compiled.graph->plan(), compiled.graph->renderSettings()};
  talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
  require(!publicPrepared.ready() &&
              !talas::runtime::renderOffline(publicCopy, 4).ok(),
          "public graph reconstruction must not mint Supported Ramp authority");
}

void testScheduledParity() {
  const auto compiled = talas::runtime::compileProject(
      makeDocument(), talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(compiled.ok(), "scheduled Ramp authority graph should compile");
  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  std::array<double, 8> actual{};
  require(offline.ok() && prepared.ready(),
          "scheduled Ramp authority should prepare in both renderers");
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto first = prepared.render(std::span<double>{actual}.first(4));
  const auto second = prepared.render(std::span<double>{actual}.subspan(4));
  countAllocations.store(false, std::memory_order_relaxed);
  require(first && second && allocationCount.load(std::memory_order_relaxed) == 0 &&
              std::ranges::equal(actual, offline.mono),
          "scheduled Ramp should render with offline/prepared parity and no "
          "callback allocation");

  const auto resetGraph = compile();
  talas::runtime::RealtimeRenderSession candidate{*resetGraph.graph};
  talas::runtime::RealtimeRenderSession pristine{*resetGraph.graph};
  std::array<double, 4> scratch{};
  require(candidate.ready() && pristine.ready() && candidate.render(scratch),
          "Ramp reset fixture should advance state");
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  candidate.resetTransport();
  countAllocations.store(false, std::memory_order_relaxed);
  std::array<double, 4> resetOutput{};
  std::array<double, 4> pristineOutput{};
  require(allocationCount.load(std::memory_order_relaxed) == 0 &&
              candidate.render(resetOutput) &&
              pristine.render(pristineOutput) &&
              resetOutput == pristineOutput,
          "Ramp reset should be allocation-free and restore exact state and "
          "schedule cursors");
}

void testAllVariantsAndConnectedAuthority() {
  for (const auto variant : std::array<std::string_view, 4>{
           "ramp", "fullRamp", "repeatingRamp", "repeatingFullRamp"}) {
    auto document = makeDocument();
    auto ramp = std::ranges::find(document.graph.nodes,
                                  std::string_view{"ramp"},
                                  &talas::project::ProjectNode::instanceId);
    require(ramp != document.graph.nodes.end(),
            "variant Ramp fixture should contain its node");
    ramp->parameters[2] = choiceParameter("variant", std::string{variant});
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(compiled.ok() &&
                rampStep(*compiled.graph).offlineSupport ==
                    talas::core::ExecutionSupport::Supported &&
                rampStep(*compiled.graph).preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Supported,
            std::string{variant} +
                " Ramp should retain exact Supported authority");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
    std::array<double, 8> actual{};
    require(offline.ok() && prepared.ready() &&
                prepared.render(std::span<double>{actual}.first(3)) &&
                prepared.render(std::span<double>{actual}.subspan(3, 4)) &&
                prepared.render(std::span<double>{actual}.subspan(7)) &&
                std::ranges::equal(actual, offline.mono),
            std::string{variant} +
                " Ramp should preserve lifecycle across irregular partitions");
  }

  const auto connected = talas::runtime::compileProject(
      makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
      makeConnectedSettings());
  require(connected.ok() &&
              rampStep(*connected.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              rampStep(*connected.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "connected Ramp controls should retain Supported authority");
  const auto offline = talas::runtime::renderOffline(*connected.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*connected.graph};
  std::array<double, 8> actual{};
  require(offline.ok() && prepared.ready() &&
              prepared.render(std::span<double>{actual}.first(4)) &&
              prepared.render(std::span<double>{actual}.subspan(4)) &&
              std::ranges::equal(actual, offline.mono),
          "connected Ramp trigger and duration should own current-frame "
          "offline/prepared values");

  auto inertPayloadGraph = *connected.graph;
  auto &inertPayloadPlan = const_cast<talas::runtime::RuntimePlan &>(
      inertPayloadGraph.plan());
  rampStep(inertPayloadPlan).parameters[0].value = 184.0;
  rampStep(inertPayloadPlan).parameters[1].value = 184.0;
  const auto inertPayloadOffline =
      talas::runtime::renderOffline(inertPayloadGraph, 8);
  talas::runtime::RealtimeRenderSession inertPayloadPrepared{
      inertPayloadGraph};
  std::array<double, 8> inertPayloadActual{};
  require(inertPayloadOffline.ok() && inertPayloadPrepared.ready() &&
              inertPayloadPrepared.render(
                  std::span<double>{inertPayloadActual}.first(4)) &&
              inertPayloadPrepared.render(
                  std::span<double>{inertPayloadActual}.subspan(4)) &&
              std::ranges::equal(inertPayloadActual,
                                 inertPayloadOffline.mono) &&
              std::ranges::equal(inertPayloadActual, offline.mono),
          "connected Ramp inputs should make only their numeric fallback "
          "payloads inert across both exact plan comparators");

  auto inertFallbackDocument = makeConnectedDocument();
  auto inertFallbackRamp = std::ranges::find(
      inertFallbackDocument.graph.nodes, std::string_view{"ramp"},
      &talas::project::ProjectNode::instanceId);
  require(inertFallbackRamp != inertFallbackDocument.graph.nodes.end(),
          "connected inert-fallback fixture should contain Ramp");
  inertFallbackRamp->parameters[0].expression = "1e200 * 1e200";
  inertFallbackRamp->parameters[1].expression = "1e200 * 1e200 * 1 s";
  const auto inertFallback = talas::runtime::compileProject(
      inertFallbackDocument, talas::core::makeBuiltinNodeRegistry(),
      makeConnectedSettings());
  require(inertFallback.ok() &&
              rampStep(*inertFallback.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              rampStep(*inertFallback.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "connected Ramp controls should hide non-finite inert fallbacks");
  const auto inertFallbackOffline =
      talas::runtime::renderOffline(*inertFallback.graph, 8);
  talas::runtime::RealtimeRenderSession inertFallbackPrepared{
      *inertFallback.graph};
  std::array<double, 8> inertFallbackActual{};
  require(inertFallbackOffline.ok(),
          "connected Ramp should render offline independently of non-finite "
          "fallback payloads");
  require(inertFallbackPrepared.ready(),
          "connected Ramp with non-finite fallbacks should prepare: " +
              std::string{inertFallbackPrepared.error()});
  const auto inertFallbackFirst = inertFallbackPrepared.render(
      std::span<double>{inertFallbackActual}.first(3));
  const auto inertFallbackSecond = inertFallbackPrepared.render(
      std::span<double>{inertFallbackActual}.subspan(3, 3));
  const auto inertFallbackThird = inertFallbackPrepared.render(
      std::span<double>{inertFallbackActual}.subspan(6));
  require(inertFallbackFirst && inertFallbackSecond && inertFallbackThird,
          "connected Ramp with non-finite fallbacks should render prepared");
  require(std::ranges::equal(inertFallbackActual,
                             inertFallbackOffline.mono),
          "connected Ramp controls should render independently of non-finite "
          "fallback payloads");

  const auto requireConnectedForgeryRejected = [&connected](
      std::string_view label, const auto &mutate) {
    auto forged = *connected.graph;
    auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
    mutate(plan);
    talas::runtime::RealtimeRenderSession candidate{forged};
    require(!candidate.ready() &&
                !talas::runtime::renderOffline(forged, 4).ok(),
            "forged connected " + std::string{label} +
                " Ramp authority must fail in both renderers");
  };
  requireConnectedForgeryRejected("producer", [](auto &plan) {
    rampStep(plan).inputs.front().sourceNodeId = "C184ForgedProducer";
  });
  requireConnectedForgeryRejected("source buffer", [](auto &plan) {
    const auto bufferIndex = rampStep(plan).inputs.front().bufferIndex;
    require(bufferIndex < plan.buffers.size(),
            "connected Ramp input should own a source buffer");
    plan.buffers[bufferIndex].frames -= 1;
  });
}

void testDynamicFallbackIsHonestSubset() {
  auto document = makeDocument();
  document.expressionLanguage =
      talas::expr::ExpressionLanguageVersion::VariablesV1;
  document.variableLibrary.variables.push_back({
      "rampDuration",
      "Ramp duration",
      talas::expr::ValueDomain::Scalar,
      0.5,
      {talas::project::ProjectVariableOwnerKind::Project, {}},
      true,
  });
  auto ramp = std::ranges::find(document.graph.nodes,
                                std::string_view{"ramp"},
                                &talas::project::ProjectNode::instanceId);
  require(ramp != document.graph.nodes.end(),
          "dynamic Ramp fixture should contain its node");
  ramp->parameters[1].expression = "@rampDuration * 1 s";
  const auto compiled = talas::runtime::compileProject(
      document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(compiled.ok(), "variable-backed Ramp fixture should compile");
  const auto &step = rampStep(*compiled.graph);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  require(step.offlineSupport == talas::core::ExecutionSupport::Subset &&
              step.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset &&
              prepared.ready() &&
              talas::runtime::renderOffline(*compiled.graph, 4).ok(),
          "variable-backed Ramp must downgrade both axes and retain "
          "compatibility");

  const auto requireDynamicDemotion = [](std::string expression,
                                         std::string_view label) {
    auto candidateDocument = makeDocument();
    candidateDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    auto candidateRamp = std::ranges::find(
        candidateDocument.graph.nodes, std::string_view{"ramp"},
        &talas::project::ProjectNode::instanceId);
    require(candidateRamp != candidateDocument.graph.nodes.end(),
            "dynamic Ramp fixture should contain its node");
    candidateRamp->parameters[0].expression = std::move(expression);
    const auto result = talas::runtime::compileProject(
        candidateDocument, talas::core::makeBuiltinNodeRegistry(),
        makeSettings());
    require(result.ok(), std::string{label} + " Ramp fixture should compile");
    const auto &candidateStep = rampStep(*result.graph);
    talas::runtime::RealtimeRenderSession candidatePrepared{*result.graph};
    require(candidateStep.offlineSupport ==
                    talas::core::ExecutionSupport::Subset &&
                candidateStep.preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Subset &&
                candidatePrepared.ready() &&
                talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label} +
                " Ramp should downgrade both axes and retain compatibility");
  };
  requireDynamicDemotion("previous(0.5, 0.5)", "stateful");
  requireDynamicDemotion("[pi, 0.5][1]", "identifier-bearing");
}

void testNestedAndMidiVoiceScopeAuthority() {
  talas::project::ProjectSoundDefinition nestedDefinition;
  nestedDefinition.definitionId = "C184NestedRamp";
  nestedDefinition.displayName = "C184 Nested Ramp";
  nestedDefinition.outputs.push_back(
      {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
  nestedDefinition.graph.nodes = {
      {"ramp",
       "talas.ramp",
       {
           {"trigger", "1"},
           {"duration", "0.5 s"},
           choiceParameter("variant", "fullRamp"),
       }},
      {"carrier",
       "talas.oscillator",
       {
           {"frequency", "1 hz"},
           {"amplitude", "1"},
       }},
      {"vca", "talas.vca", {}},
  };
  nestedDefinition.graph.connections = {
      {"ramp", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
  };

  talas::project::ProjectDocument nestedDocument;
  nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
  nestedDocument.graph.nodes = {
      soundInstance("nested", "C184NestedRamp"),
      {"out", "talas.audioOutput", {}},
  };
  nestedDocument.graph.connections = {
      {"nested", "audioOut", "out", "audioIn"},
  };
  const talas::runtime::RenderSettings nestedSettings{8.0, 8};
  const auto nested = talas::runtime::compileProject(
      nestedDocument, talas::core::makeBuiltinNodeRegistry(), nestedSettings);
  require(nested.ok(), "nested Ramp authority graph should compile");
  const auto &nestedRamp = rampStep(*nested.graph);
  require(nestedRamp.scopeId != talas::runtime::globalRuntimeScope &&
              nestedRamp.eventValueScopeId == nestedRamp.scopeId &&
              nestedRamp.offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              nestedRamp.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "nested Ramp should retain Supported authority in its "
          "compiler-owned structural scope");
  const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
  talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
  std::array<double, 8> nestedActual{};
  require(nestedOffline.ok() && nestedPrepared.ready() &&
              nestedPrepared.render(nestedActual) &&
              std::ranges::equal(nestedActual, nestedOffline.mono),
          "nested Ramp should render with offline/prepared parity");

  auto forgedNested = *nested.graph;
  auto &forgedNestedPlan =
      const_cast<talas::runtime::RuntimePlan &>(forgedNested.plan());
  rampStep(forgedNestedPlan).scopeId = talas::runtime::globalRuntimeScope;
  talas::runtime::RealtimeRenderSession forgedNestedPrepared{forgedNested};
  require(!forgedNestedPrepared.ready() &&
              !talas::runtime::renderOffline(forgedNested, 4).ok(),
          "forged nested Ramp topology must fail in both renderers");

  talas::project::ProjectDocument midiDocument;
  midiDocument.graph.nodes = {
      {"ramp",
       "talas.ramp",
       {
           {"trigger", "1"},
           {"duration", "0.5 s"},
           choiceParameter("variant", "repeatingRamp"),
       }},
      {"carrier",
       "talas.oscillator",
       {
           {"frequency", "440 hz"},
           {"amplitude", "1"},
       }},
      {"vca", "talas.vca", {}},
      {"midi",
       "talas.midiVoice",
       {
           {"left", "1"},
           {"right", "1"},
           {"channel", "1"},
           textParameter("source", ""),
           resourceParameter("midiFile", ""),
           {"polyphony", "2"},
           {"lowPitch", "0 nn"},
           {"highPitch", "127 nn"},
           textParameter("script", ""),
           booleanParameter("shared", false),
       }},
      {"out", "talas.audioOutput", {}},
  };
  midiDocument.graph.connections = {
      {"ramp", "value", "vca", "gain"},
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
          "MIDI Voice Ramp authority graph should compile two voices");
  const auto &midiRegion = midi.graph->plan().midiVoices.front();
  for (const auto &voice : midiRegion.voices) {
    const auto first = midi.graph->plan().steps.begin() +
                       static_cast<std::ptrdiff_t>(voice.firstStepIndex);
    const auto last = first + static_cast<std::ptrdiff_t>(voice.stepCount);
    const auto ramp = std::ranges::find(
        first, last, std::string_view{"talas.ramp"},
        &talas::runtime::CompileStep::typeId);
    require(ramp != last && ramp->scopeId == voice.scopeId &&
                ramp->structuralScopeId == talas::runtime::globalRuntimeScope &&
                ramp->eventValueScopeId == ramp->structuralScopeId &&
                ramp->eventValueScopeId != ramp->scopeId &&
                ramp->offlineSupport ==
                    talas::core::ExecutionSupport::Supported &&
                ramp->offlineBinding ==
                    talas::core::ExecutionBinding::LegacyAdapter &&
                ramp->preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Supported &&
                ramp->preparedRealtimeBinding ==
                    talas::core::ExecutionBinding::LegacyAdapter,
            "each MIDI Voice clone should retain exact Supported Ramp "
            "authority in its compiler-owned voice scope");
  }
  const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
  talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
  std::array<double, 16> midiActual{};
  require(midiOffline.ok() && midiPrepared.ready() &&
              midiPrepared.render(midiActual) &&
              std::ranges::equal(midiActual, midiOffline.mono),
          "MIDI Voice Ramp clones should render with offline/prepared parity");
}

void testCustomCompatibilityAndAuthorityRejection() {
  const auto exact = compile();
  require(exact.ok(), "exact Ramp authority fixture should compile");

  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport(
      "talas.ramp", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.ramp", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  const auto subset = compile(std::move(subsetRegistry));
  require(subset.ok(), "custom both-Subset Ramp should compile");
  talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
  require(subsetPrepared.ready() &&
              talas::runtime::renderOffline(*subset.graph, 4).ok(),
          "custom both-Subset Ramp should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.ramp", talas::core::FanInLatencyPolicy::NotApplicable);
  const auto divergent = compile(std::move(divergentRegistry));
  require(divergent.ok(),
          "divergent Ramp descriptor should still compile a plan");
  talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
  require(!divergentPrepared.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
          "divergent descriptor must not mint Supported Ramp authority");

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
                " Ramp authority must fail in both renderers");
  };
  requireForgeryRejected(
      "settings", [](auto &, auto &settings) { settings.maxBlockFrames += 1; });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C184Forged", {0.0}});
  });
  requireForgeryRejected("support", [](auto &plan, auto &) {
    rampStep(plan).preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    rampStep(plan).preparedRealtimeBinding =
        talas::core::ExecutionBinding::Kernel;
  });
  requireForgeryRejected("reset", [](auto &plan, auto &) {
    rampStep(plan).supportsReset = false;
  });
  requireForgeryRejected(
      "scope", [](auto &plan, auto &) { rampStep(plan).scopeId = 184; });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    rampOutput(plan).nodeId = "C184ForgedOwner";
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    rampOutput(plan).frames -= 1;
  });
  requireForgeryRejected("duplicate output", [](auto &plan, auto &) {
    plan.buffers.push_back(rampOutput(plan));
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    rampStep(plan).resolvedLatencyFrames = 1;
  });
  requireForgeryRejected("event scope", [](auto &plan, auto &) {
    rampStep(plan).eventValueScopeId += 1;
  });
  requireForgeryRejected("kernel", [](auto &plan, auto &) {
    rampStep(plan).kernelKind = talas::runtime::KernelKind::Gate;
  });
  requireForgeryRejected("isolation", [](auto &plan, auto &) {
    rampStep(plan).preparedIsolatedComponent = true;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    rampStep(plan).parameters.front().domain = talas::expr::ValueDomain::Seconds;
  });
  requireForgeryRejected("variant", [](auto &plan, auto &) {
    rampStep(plan).parameters[2].choice = "C184ForgedVariant";
  });
}
} // namespace

int main() {
  testExactCapabilityAndPrivateCompilerAuthority();
  testScheduledParity();
  testAllVariantsAndConnectedAuthority();
  testDynamicFallbackIsHonestSubset();
  testNestedAndMidiVoiceScopeAuthority();
  testCustomCompatibilityAndAuthorityRejection();
  return 0;
}
