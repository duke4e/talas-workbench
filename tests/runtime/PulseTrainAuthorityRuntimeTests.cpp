#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
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

namespace talas::runtime {
struct OfflineRenderSessionTestAccess {
  [[nodiscard]] static std::uint64_t
  exchangeRejectedTransactionEndFrame(std::uint64_t absoluteEndFrame) noexcept {
    return OfflineRenderSession::exchangeRejectedTransactionEndFrameForTesting(
        absoluteEndFrame);
  }
};
} // namespace talas::runtime

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
      {"pulse",
       "talas.pulseTrain",
       {
           {"period", "!C183Period * 1 s"},
           booleanParameter("variableDutyCycle", true),
           {"dutyCycle", "!C183Duty"},
           {"gate", "!C183Gate"},
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
      {"pulse", "value", "vca", "gain"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::runtime::RenderSettings makeSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C183Period", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183Duty", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183Gate", 1.0, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {2, "C183Period", 0.25, talas::expr::ValueDomain::Scalar},
      {2, "C183Duty", 1.0, talas::expr::ValueDomain::Scalar},
      {4, "C183Gate", 0.0, talas::expr::ValueDomain::Scalar},
      {6, "C183Gate", 1.0, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::project::ProjectDocument makeConnectedDocument() {
  auto document = makeDocument();
  auto pulse =
      std::ranges::find(document.graph.nodes, std::string_view{"pulse"},
                        &talas::project::ProjectNode::instanceId);
  require(pulse != document.graph.nodes.end(),
          "connected Pulse Train fixture should contain its node");
  pulse->parameters = {
      {"period", "1 s"},
      booleanParameter("variableDutyCycle", true),
      {"dutyCycle", "0.5"},
      {"gate", "0"},
  };
  document.graph.nodes.insert(document.graph.nodes.begin(),
                              {
                                  {"periodControl",
                                   "talas.constant",
                                   {
                                       {"value", "!C183ConnectedPeriod"},
                                   }},
                                  {"dutyControl",
                                   "talas.constant",
                                   {
                                       {"value", "!C183ConnectedDuty"},
                                   }},
                                  {"gateControl",
                                   "talas.constant",
                                   {
                                       {"value", "!C183ConnectedGate"},
                                   }},
                              });
  document.graph.connections.insert(
      document.graph.connections.begin(),
      {
          {"periodControl", "value", "pulse", "period"},
          {"dutyControl", "value", "pulse", "dutyCycle"},
          {"gateControl", "value", "pulse", "gate"},
      });
  return document;
}

talas::runtime::RenderSettings makeConnectedSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C183ConnectedPeriod", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183ConnectedDuty", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183ConnectedGate", 1.0, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {2, "C183ConnectedPeriod", 0.25, talas::expr::ValueDomain::Scalar},
      {2, "C183ConnectedDuty", 1.0, talas::expr::ValueDomain::Scalar},
      {4, "C183ConnectedGate", 0.0, talas::expr::ValueDomain::Scalar},
      {6, "C183ConnectedGate", 1.0, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::project::ProjectDocument makeRollbackDocument() {
  auto document = makeDocument();
  document.graph.nodes.push_back({"failureGain",
                                  "talas.constant",
                                  {
                                      {"value", "!C183Downstream"},
                                  }});
  document.graph.nodes.push_back(
      {"doubleGain", "talas.constant", {{"value", "2"}}});
  document.graph.nodes.push_back({"failureProduct", "talas.product", {}});
  document.graph.nodes.push_back({"failureOverflow", "talas.product", {}});
  auto vcaGain = std::ranges::find_if(
      document.graph.connections, [](const auto &connection) {
        return connection.toNode == "vca" && connection.toPort == "gain";
      });
  require(vcaGain != document.graph.connections.end(),
          "rollback Pulse Train fixture should contain VCA gain binding");
  document.graph.connections.erase(vcaGain);
  document.graph.connections.push_back(
      {"pulse", "value", "failureProduct", "left"});
  document.graph.connections.push_back(
      {"failureGain", "value", "failureProduct", "right"});
  document.graph.connections.push_back(
      {"failureProduct", "value", "failureOverflow", "left"});
  document.graph.connections.push_back(
      {"doubleGain", "value", "failureOverflow", "right"});
  document.graph.connections.push_back(
      {"failureOverflow", "value", "vca", "gain"});
  return document;
}

talas::runtime::RenderSettings makeRollbackSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C183Period", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183Duty", 0.5, talas::expr::ValueDomain::Scalar},
      {"C183Gate", 1.0, talas::expr::ValueDomain::Scalar},
      {"C183Downstream", 1.0, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::runtime::CompileResult
compile(talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry()) {
  return talas::runtime::compileProject(makeDocument(), std::move(registry),
                                     makeSettings());
}

const talas::runtime::CompileStep &
pulseStep(const talas::runtime::PreparedRuntimeGraph &graph) {
  const auto found =
      std::ranges::find(graph.plan().steps, std::string_view{"talas.pulseTrain"},
                        &talas::runtime::CompileStep::typeId);
  require(found != graph.plan().steps.end(),
          "Pulse Train authority fixture should contain its step");
  return *found;
}

talas::runtime::CompileStep &pulseStep(talas::runtime::RuntimePlan &plan) {
  const auto found =
      std::ranges::find(plan.steps, std::string_view{"talas.pulseTrain"},
                        &talas::runtime::CompileStep::typeId);
  require(found != plan.steps.end(),
          "Pulse Train authority forgery should contain its step");
  return *found;
}

talas::runtime::BufferSpec &pulseOutput(talas::runtime::RuntimePlan &plan) {
  const auto found = std::ranges::find_if(plan.buffers, [](const auto &buffer) {
    return buffer.nodeId == "pulse" && buffer.portId == "value" &&
           buffer.type == talas::core::PortType::Control;
  });
  require(found != plan.buffers.end(),
          "Pulse Train authority forgery should contain its output");
  return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.pulseTrain");
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
  const auto exactBoolean = [](const talas::core::ParameterDescriptor &parameter,
                               std::string_view id) {
    return parameter.id == id &&
           parameter.kind == talas::core::ParameterKind::Boolean &&
           parameter.domain == talas::expr::ValueDomain::Scalar &&
           parameter.defaultExpression.empty() &&
           parameter.defaultChoice.empty() && parameter.choices.empty() &&
           parameter.defaultBoolean == std::optional<bool>{false} &&
           parameter.defaultResource.empty() && parameter.defaultText.empty() &&
           parameter.resourceAuthority ==
               talas::core::ParameterResourceAuthority::None;
  };
  const std::vector<std::string> limitations{
      "offline-and-prepared-matched-checked-finite-absolute-elapsed-time-pulse-"
      "train",
      "static-direct-event-value-or-per-frame-connected-positive-period-"
      "selected-bounded-duty-and-finite-gate",
      "exact-pre-increment-pulse-post-increment-fold-gate-close-positive-zero-"
      "reset-and-immediate-reopen",
      "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-"
      "binding-buffer-producer-scope-zero-latency-and-local-transaction-"
      "authority",
      "allocation-free-prepared-callback-failure-atomic-elapsed-schedule-"
      "output-retry-and-reset",
      "band-limited-phase-offset-sync-tempo-typed-trigger-multichannel-and-"
      "exact-reference-pulse-edge-semantics-unsupported",
  };
  require(descriptor != nullptr && descriptor->displayName == "Pulse Train" &&
              descriptor->category == "Xtra Sources" &&
              descriptor->supportLevel ==
                  talas::core::NodeSupportLevel::FullRuntime &&
              descriptor->supportSummary ==
                  "Full runtime coverage; no tracked subset restriction." &&
              descriptor->inputs.size() == 3 &&
              exactControlPort(descriptor->inputs[0], "period") &&
              exactControlPort(descriptor->inputs[1], "dutyCycle") &&
              exactControlPort(descriptor->inputs[2], "gate") &&
              descriptor->outputs.size() == 1 &&
              exactControlPort(descriptor->outputs[0], "value") &&
              descriptor->parameters.size() == 4 &&
              exactExpression(descriptor->parameters[0], "period",
                              talas::expr::ValueDomain::Seconds, "0.5 s") &&
              exactBoolean(descriptor->parameters[1], "variableDutyCycle") &&
              exactExpression(descriptor->parameters[2], "dutyCycle",
                              talas::expr::ValueDomain::Scalar, "0.5") &&
              exactExpression(descriptor->parameters[3], "gate",
                              talas::expr::ValueDomain::Scalar, "1") &&
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
          "exact Pulse Train descriptor should publish complete Supported "
          "authority");

  const auto compiled = compile();
  require(compiled.ok(), "exact Pulse Train authority graph should compile");
  const auto &step = pulseStep(*compiled.graph);
  require(
      step.offlineSupport == talas::core::ExecutionSupport::Supported &&
          step.offlineBinding == talas::core::ExecutionBinding::LegacyAdapter &&
          step.preparedRealtimeSupport ==
              talas::core::ExecutionSupport::Supported &&
          step.preparedRealtimeBinding ==
              talas::core::ExecutionBinding::LegacyAdapter &&
          step.supportsReset && !step.kernelKind.has_value() &&
          !step.preparedIsolatedComponent && step.resolvedLatencyFrames == 0 &&
          step.cumulativeInputLatencyFrames == 0 &&
          step.cumulativeOutputLatencyFrames == 0,
      "compiler should retain exact promoted Pulse Train plan authority");

  talas::runtime::PreparedRuntimeGraph publicCopy{
      compiled.graph->plan(), compiled.graph->renderSettings()};
  talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
  require(!publicPrepared.ready() &&
              !talas::runtime::renderOffline(publicCopy, 4).ok(),
          "public graph reconstruction must not mint Supported Pulse Train "
          "authority");
}

void testScheduledParityPartitionsAllocationAndReset() {
  const auto compiled = compile();
  require(compiled.ok(),
          "scheduled Pulse Train authority graph should compile");
  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  require(offline.ok() && prepared.ready(),
          "exact Pulse Train should prepare in both renderers");

  std::array<double, 8> actual{};
  std::size_t offset = 0;
  for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2}) {
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered =
        prepared.render(std::span<double>{actual}.subspan(offset, frames));
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "Pulse Train prepared partitions should render without allocation");
    offset += frames;
  }
  require(std::ranges::equal(actual, offline.mono) &&
              std::ranges::any_of(actual,
                                  [](double value) { return value != 0.0; }),
          "Pulse Train prepared schedules should be nontrivial and "
          "bit-identical offline");

  const auto resetGraph = compile();
  talas::runtime::RealtimeRenderSession candidate{*resetGraph.graph};
  talas::runtime::RealtimeRenderSession pristine{*resetGraph.graph};
  std::array<double, 4> scratch{};
  require(
      candidate.ready() && pristine.ready() && candidate.render(scratch),
      "Pulse Train reset fixtures should prepare and advance elapsed state");
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  candidate.resetTransport();
  countAllocations.store(false, std::memory_order_relaxed);
  require(allocationCount.load(std::memory_order_relaxed) == 0,
          "Pulse Train transport reset should not allocate");
  std::array<double, 4> resetOutput{};
  std::array<double, 4> pristineOutput{};
  require(candidate.render(resetOutput) && pristine.render(pristineOutput) &&
              resetOutput == pristineOutput,
          "Pulse Train transport reset should restore elapsed state and "
          "schedule cursors");
}

void testConnectedControlsAndInertDutyAuthority() {
  const auto compiled = talas::runtime::compileProject(
      makeConnectedDocument(), talas::core::makeBuiltinNodeRegistry(),
      makeConnectedSettings());
  require(compiled.ok(),
          "connected Pulse Train authority graph should compile");
  const auto &step = pulseStep(*compiled.graph);
  require(step.inputs.size() == 3 &&
              step.offlineSupport == talas::core::ExecutionSupport::Supported &&
              step.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "all three connected Pulse Train controls should retain promoted "
          "authority");

  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  std::array<double, 8> actual{};
  require(offline.ok() && prepared.ready() &&
              prepared.render(std::span<double>{actual}.first(4)) &&
              prepared.render(std::span<double>{actual}.subspan(4)) &&
              std::ranges::equal(actual, offline.mono) &&
              std::ranges::any_of(actual,
                                  [](double value) { return value != 0.0; }),
          "connected period, duty, and gate should drive current-frame "
          "prepared output with offline parity");

  auto inertDocument = makeConnectedDocument();
  auto pulse =
      std::ranges::find(inertDocument.graph.nodes, std::string_view{"pulse"},
                        &talas::project::ProjectNode::instanceId);
  require(pulse != inertDocument.graph.nodes.end(),
          "inert-duty fixture should contain Pulse Train");
  pulse->parameters[1] = booleanParameter("variableDutyCycle", false);
  auto inertSettings = makeConnectedSettings();
  inertSettings.eventValues[1].value = 1.5;
  inertSettings.eventValueChanges.erase(
      inertSettings.eventValueChanges.begin() + 1);
  const auto inert = talas::runtime::compileProject(
      inertDocument, talas::core::makeBuiltinNodeRegistry(), inertSettings);
  require(inert.ok() && pulseStep(*inert.graph).preparedRealtimeSupport ==
                            talas::core::ExecutionSupport::Supported,
          "disabled variable duty should retain Supported authority with an "
          "inert out-of-range connected value");
  const auto inertOffline = talas::runtime::renderOffline(*inert.graph, 4);
  talas::runtime::RealtimeRenderSession inertPrepared{*inert.graph};
  std::array<double, 4> inertActual{};
  require(inertOffline.ok() && inertPrepared.ready() &&
              inertPrepared.render(inertActual) &&
              std::ranges::equal(inertActual, inertOffline.mono),
          "disabled variable duty should ignore its connected value in both "
          "renderers");

  const auto requireConnectedForgeryRejected =
      [&compiled](std::string_view label, const auto &mutate) {
        auto forged = *compiled.graph;
        auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
        mutate(plan);
        talas::runtime::RealtimeRenderSession candidate{forged};
        require(!candidate.ready() &&
                    !talas::runtime::renderOffline(forged, 4).ok(),
                "forged connected " + std::string{label} +
                    " Pulse Train authority must fail in both renderers");
      };
  requireConnectedForgeryRejected("producer", [](auto &plan) {
    pulseStep(plan).inputs.front().sourceNodeId = "C183ForgedProducer";
  });
  requireConnectedForgeryRejected("source buffer", [](auto &plan) {
    const auto bufferIndex = pulseStep(plan).inputs.front().bufferIndex;
    require(bufferIndex < plan.buffers.size(),
            "connected Pulse Train input should own a source buffer");
    plan.buffers[bufferIndex].frames -= 1;
  });
}

void testScheduledFailureRollsBackElapsedAndEventCursors() {
  auto offlineSettings = makeRollbackSettings();
  offlineSettings.eventValueChanges = {
      {2, "C183Period", 0.375, talas::expr::ValueDomain::Scalar},
      {2, "C183Duty", 0.5, talas::expr::ValueDomain::Scalar},
      {2, "C183Gate", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto offlineGraph = talas::runtime::compileProject(
      makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
      offlineSettings);
  require(offlineGraph.ok(),
          "offline scheduled Pulse Train rollback graph should compile");
  talas::runtime::OfflineRenderSession candidate{*offlineGraph.graph};
  const auto warm = candidate.render(2);
  const auto previousInjectedEndFrame = talas::runtime::
      OfflineRenderSessionTestAccess::exchangeRejectedTransactionEndFrame(4);
  const auto rejected = candidate.render(2);
  const auto repeated = candidate.render(2);
  const auto disabledInjectedEndFrame = talas::runtime::
      OfflineRenderSessionTestAccess::exchangeRejectedTransactionEndFrame(0);
  const auto repaired = candidate.render(2);

  const auto referenceGraph = talas::runtime::compileProject(
      makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
      offlineSettings);
  require(referenceGraph.ok(),
          "offline Pulse Train rollback reference should compile");
  talas::runtime::OfflineRenderSession reference{*referenceGraph.graph};
  const auto expectedWarm = reference.render(2);
  const auto expected = reference.render(2);
  talas::runtime::OfflineRenderSession leakedStateReference{
      *referenceGraph.graph};
  const auto leakedWarm = leakedStateReference.render(2);
  const auto leakedRejectedRange = leakedStateReference.render(2);
  const auto leakedRetry = leakedStateReference.render(2);
  require(previousInjectedEndFrame == 0 && disabledInjectedEndFrame == 4 &&
              warm.ok() && expectedWarm.ok() &&
              warm.mono == expectedWarm.mono && !rejected.ok() &&
              rejected.mono.empty() && !repeated.ok() &&
              repeated.mono.empty() && repaired.ok() && expected.ok() &&
              leakedWarm.ok() && leakedRejectedRange.ok() &&
              leakedRetry.ok() && leakedRetry.mono != expected.mono &&
              repaired.mono == expected.mono,
          "offline failure should restore Pulse Train elapsed state and all "
          "same-frame direct EventValue cursors before a continuation that "
          "distinguishes correct rollback from leaked elapsed state");

  const auto realtimeGraph = talas::runtime::compileProject(
      makeRollbackDocument(), talas::core::makeBuiltinNodeRegistry(),
      makeRollbackSettings());
  require(realtimeGraph.ok(),
          "prepared scheduled Pulse Train rollback graph should compile");
  talas::runtime::RealtimeRenderSession preparedCandidate{*realtimeGraph.graph};
  talas::runtime::RealtimeRenderSession preparedReference{*realtimeGraph.graph};
  require(preparedCandidate.ready() && preparedReference.ready(),
          "prepared Pulse Train rollback fixtures should prepare");
  const auto schedulePulse = [](auto &session) {
    return session.scheduleEventValue("C183Period", 0.375,
                                      talas::expr::ValueDomain::Scalar, 1) &&
           session.scheduleEventValue("C183Duty", 0.5,
                                      talas::expr::ValueDomain::Scalar, 1) &&
           session.scheduleEventValue("C183Gate", 1.0,
                                      talas::expr::ValueDomain::Scalar, 1);
  };
  require(
      schedulePulse(preparedCandidate) && schedulePulse(preparedReference) &&
          preparedCandidate.scheduleEventValue(
              "C183Downstream", std::numeric_limits<double>::max(),
              talas::expr::ValueDomain::Scalar, 3),
      "prepared Pulse Train rollback fixture should accept bounded schedules");
  std::array<double, 4> rejectedOutput;
  rejectedOutput.fill(183.0);
  require(!preparedCandidate.render(rejectedOutput) &&
              rejectedOutput == std::array<double, 4>{},
          "prepared downstream failure should publish no callback prefix");
  rejectedOutput.fill(183.0);
  require(!preparedCandidate.render(rejectedOutput) &&
              rejectedOutput == std::array<double, 4>{},
          "prepared Pulse Train retry should repeat rejection from restored "
          "cursors");
  require(preparedCandidate.scheduleEventValue(
              "C183Downstream", 1.0, talas::expr::ValueDomain::Scalar, 3) &&
              preparedReference.scheduleEventValue(
                  "C183Downstream", std::numeric_limits<double>::max(),
                  talas::expr::ValueDomain::Scalar, 3) &&
              preparedReference.scheduleEventValue(
                  "C183Downstream", 1.0, talas::expr::ValueDomain::Scalar, 3),
          "prepared Pulse Train rollback fixture should accept same-frame "
          "repair ordering");
  std::array<double, 4> preparedRepaired{};
  std::array<double, 4> preparedExpected{};
  require(preparedCandidate.render(preparedRepaired) &&
              preparedReference.render(preparedExpected) &&
              preparedRepaired == preparedExpected,
          "prepared repair should replay exact Pulse Train elapsed state and "
          "schedules from the failed callback boundary");
}

void testNestedAndMidiVoiceScopeAuthority() {
  talas::project::ProjectSoundDefinition nestedDefinition;
  nestedDefinition.definitionId = "C183NestedPulse";
  nestedDefinition.displayName = "C183 Nested Pulse";
  nestedDefinition.outputs.push_back(
      {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
  nestedDefinition.graph.nodes = {
      {"pulse",
       "talas.pulseTrain",
       {
           {"period", "0.375 s"},
           booleanParameter("variableDutyCycle", true),
           {"dutyCycle", "0.5"},
           {"gate", "1"},
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
      {"pulse", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
  };

  talas::project::ProjectDocument nestedDocument;
  nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
  nestedDocument.graph.nodes = {
      soundInstance("nested", "C183NestedPulse"),
      {"out", "talas.audioOutput", {}},
  };
  nestedDocument.graph.connections = {
      {"nested", "audioOut", "out", "audioIn"},
  };
  const talas::runtime::RenderSettings nestedSettings{8.0, 8};
  const auto nested = talas::runtime::compileProject(
      nestedDocument, talas::core::makeBuiltinNodeRegistry(), nestedSettings);
  require(nested.ok(), "nested Pulse Train authority graph should compile");
  const auto &nestedPulse = pulseStep(*nested.graph);
  require(nestedPulse.scopeId != talas::runtime::globalRuntimeScope &&
              nestedPulse.eventValueScopeId == nestedPulse.scopeId &&
              nestedPulse.offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              nestedPulse.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "nested Pulse Train should retain Supported authority in its "
          "compiler-owned structural scope");
  const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
  talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
  std::array<double, 8> nestedActual{};
  require(nestedOffline.ok() && nestedPrepared.ready() &&
              nestedPrepared.render(nestedActual) &&
              std::ranges::equal(nestedActual, nestedOffline.mono),
          "nested Pulse Train should render with offline/prepared parity");

  auto forgedNested = *nested.graph;
  auto &forgedNestedPlan =
      const_cast<talas::runtime::RuntimePlan &>(forgedNested.plan());
  pulseStep(forgedNestedPlan).scopeId = talas::runtime::globalRuntimeScope;
  talas::runtime::RealtimeRenderSession forgedNestedPrepared{forgedNested};
  require(!forgedNestedPrepared.ready() &&
              !talas::runtime::renderOffline(forgedNested, 4).ok(),
          "forged nested Pulse Train topology must fail in both renderers");

  talas::project::ProjectDocument midiDocument;
  midiDocument.graph.nodes = {
      {"pulse",
       "talas.pulseTrain",
       {
           {"period", "0.375 s"},
           booleanParameter("variableDutyCycle", true),
           {"dutyCycle", "0.5"},
           {"gate", "1"},
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
      {"pulse", "value", "vca", "gain"},
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
          "MIDI Voice Pulse Train authority graph should compile two voices");
  const auto &midiRegion = midi.graph->plan().midiVoices.front();
  for (const auto &voice : midiRegion.voices) {
    const auto first = midi.graph->plan().steps.begin() +
                       static_cast<std::ptrdiff_t>(voice.firstStepIndex);
    const auto last = first + static_cast<std::ptrdiff_t>(voice.stepCount);
    const auto pulse = std::ranges::find(
        first, last, std::string_view{"talas.pulseTrain"},
        &talas::runtime::CompileStep::typeId);
    require(pulse != last && pulse->scopeId == voice.scopeId &&
                pulse->structuralScopeId == talas::runtime::globalRuntimeScope &&
                pulse->eventValueScopeId == pulse->structuralScopeId &&
                pulse->eventValueScopeId != pulse->scopeId &&
                pulse->offlineSupport ==
                    talas::core::ExecutionSupport::Supported &&
                pulse->offlineBinding ==
                    talas::core::ExecutionBinding::LegacyAdapter &&
                pulse->preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Supported &&
                pulse->preparedRealtimeBinding ==
                    talas::core::ExecutionBinding::LegacyAdapter,
            "each MIDI Voice clone should retain Supported Pulse Train "
            "authority in its compiler-owned voice scope");
  }
  const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
  talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
  std::array<double, 16> midiActual{};
  require(midiOffline.ok() && midiPrepared.ready() &&
              midiPrepared.render(midiActual) &&
              std::ranges::equal(midiActual, midiOffline.mono),
          "MIDI Voice Pulse Train clones should render with "
          "offline/prepared parity");
}

void testDynamicFallbackIsHonestSubset() {
  auto document = makeDocument();
  document.expressionLanguage =
      talas::expr::ExpressionLanguageVersion::VariablesV1;
  document.variableLibrary.variables.push_back({
      "pulseDuty",
      "Pulse duty",
      talas::expr::ValueDomain::Scalar,
      0.5,
      {talas::project::ProjectVariableOwnerKind::Project, {}},
      true,
  });
  auto pulse =
      std::ranges::find(document.graph.nodes, std::string_view{"pulse"},
                        &talas::project::ProjectNode::instanceId);
  require(pulse != document.graph.nodes.end(),
          "dynamic Pulse Train fixture should contain its node");
  pulse->parameters[2].expression = "@pulseDuty";
  const auto compiled = talas::runtime::compileProject(
      document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(compiled.ok(), "variable-backed Pulse Train fixture should compile");
  const auto &step = pulseStep(*compiled.graph);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  require(step.offlineSupport == talas::core::ExecutionSupport::Subset &&
              step.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset &&
              prepared.ready() &&
              talas::runtime::renderOffline(*compiled.graph, 4).ok(),
          "variable-backed Pulse Train must downgrade both axes and retain "
          "compatibility");

  const auto requireDynamicDemotion = [](std::string expression,
                                         std::string_view label) {
    auto candidateDocument = makeDocument();
    candidateDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    auto candidatePulse = std::ranges::find(
        candidateDocument.graph.nodes, std::string_view{"pulse"},
        &talas::project::ProjectNode::instanceId);
    require(candidatePulse != candidateDocument.graph.nodes.end(),
            "dynamic Pulse Train fixture should contain its node");
    candidatePulse->parameters[2].expression = std::move(expression);
    const auto result = talas::runtime::compileProject(
        candidateDocument, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(result.ok(),
            std::string{label} + " Pulse Train fixture should compile");
    const auto &candidateStep = pulseStep(*result.graph);
    talas::runtime::RealtimeRenderSession candidatePrepared{*result.graph};
    require(
        candidateStep.offlineSupport == talas::core::ExecutionSupport::Subset &&
            candidateStep.preparedRealtimeSupport ==
                talas::core::ExecutionSupport::Subset &&
            candidatePrepared.ready() &&
            talas::runtime::renderOffline(*result.graph, 4).ok(),
        std::string{label} +
            " Pulse Train should downgrade both axes and retain compatibility");
  };
  requireDynamicDemotion("previous(0.5, 0.5)", "stateful");
  requireDynamicDemotion("[pi, 0.5][1]", "identifier-bearing");
}

void testCustomCompatibilityAndAuthorityRejection() {
  const auto exact = compile();
  require(exact.ok(), "exact Pulse Train authority fixture should compile");

  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport(
      "talas.pulseTrain", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.pulseTrain", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  const auto subset = compile(std::move(subsetRegistry));
  require(subset.ok(), "custom both-Subset Pulse Train should compile");
  talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
  require(subsetPrepared.ready() &&
              talas::runtime::renderOffline(*subset.graph, 4).ok(),
          "custom both-Subset Pulse Train should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.pulseTrain", talas::core::FanInLatencyPolicy::NotApplicable);
  const auto divergent = compile(std::move(divergentRegistry));
  require(divergent.ok(),
          "divergent Pulse Train descriptor should still compile a plan");
  talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
  require(!divergentPrepared.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
          "divergent descriptor must not mint Supported Pulse Train authority");

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
                " Pulse Train authority must fail in both renderers");
  };
  requireForgeryRejected(
      "settings", [](auto &, auto &settings) { settings.maxBlockFrames += 1; });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C183Forged", {0.0}});
  });
  requireForgeryRejected("support", [](auto &plan, auto &) {
    pulseStep(plan).preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    pulseStep(plan).preparedRealtimeBinding =
        talas::core::ExecutionBinding::Kernel;
  });
  requireForgeryRejected("reset", [](auto &plan, auto &) {
    pulseStep(plan).supportsReset = false;
  });
  requireForgeryRejected(
      "scope", [](auto &plan, auto &) { pulseStep(plan).scopeId = 183; });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    pulseOutput(plan).nodeId = "C183ForgedOwner";
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    pulseOutput(plan).frames -= 1;
  });
  requireForgeryRejected("duplicate output", [](auto &plan, auto &) {
    plan.buffers.push_back(pulseOutput(plan));
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    pulseStep(plan).resolvedLatencyFrames = 1;
  });
  requireForgeryRejected("event scope", [](auto &plan, auto &) {
    pulseStep(plan).eventValueScopeId += 1;
  });
  requireForgeryRejected("kernel", [](auto &plan, auto &) {
    pulseStep(plan).kernelKind = talas::runtime::KernelKind::Gate;
  });
  requireForgeryRejected("isolation", [](auto &plan, auto &) {
    pulseStep(plan).preparedIsolatedComponent = true;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    pulseStep(plan).parameters.front().domain = talas::expr::ValueDomain::Scalar;
  });
  requireForgeryRejected("duty mode", [](auto &plan, auto &) {
    pulseStep(plan).parameters[1].boolean = false;
  });
}
} // namespace

int main() {
  testExactCapabilityAndPrivateCompilerAuthority();
  testScheduledParityPartitionsAllocationAndReset();
  testConnectedControlsAndInertDutyAuthority();
  testScheduledFailureRollsBackElapsedAndEventCursors();
  testNestedAndMidiVoiceScopeAuthority();
  testDynamicFallbackIsHonestSubset();
  testCustomCompatibilityAndAuthorityRejection();
  return 0;
}
