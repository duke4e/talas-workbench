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

talas::project::ProjectDocument makeDocument() {
  talas::project::ProjectDocument document;
  document.graph.nodes = {
      {"value", "talas.constant", {{"value", "!C181Value"}}},
      {"thresholdControl", "talas.constant", {{"value", "!C181Threshold"}}},
      {"hysteresisControl", "talas.constant", {{"value", "!C181Hysteresis"}}},
      {"threshold",
       "talas.threshold",
       {{"threshold", "0.25"}, {"hysteresis", "0.0"}}},
      {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
      {"out", "talas.audioOutput", {}},
  };
  document.graph.connections = {
      {"value", "value", "threshold", "value"},
      {"thresholdControl", "value", "threshold", "threshold"},
      {"hysteresisControl", "value", "threshold", "hysteresis"},
      {"carrier", "audioOut", "vca", "audioIn"},
      {"threshold", "value", "vca", "gain"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::project::ProjectDocument makeDynamicFallbackDocument() {
  talas::project::ProjectDocument document;
  document.graph.nodes = {
      {"value", "talas.constant", {{"value", "0.6"}}},
      {"dynamicThreshold",
       "talas.threshold",
       {{"threshold", "!C181DynamicThreshold"}, {"hysteresis", "0"}}},
      {"staticThreshold",
       "talas.threshold",
       {{"threshold", "0.5"}, {"hysteresis", "0"}}},
      {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
      {"out", "talas.audioOutput", {}},
  };
  document.graph.connections = {
      {"value", "value", "dynamicThreshold", "value"},
      {"dynamicThreshold", "value", "staticThreshold", "value"},
      {"carrier", "audioOut", "vca", "audioIn"},
      {"staticThreshold", "value", "vca", "gain"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::runtime::RenderSettings makeSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C181Value", 0.69, talas::expr::ValueDomain::Scalar},
      {"C181Threshold", 0.5, talas::expr::ValueDomain::Scalar},
      {"C181Hysteresis", 0.2, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {1, "C181Value", 0.7, talas::expr::ValueDomain::Scalar},
      {3, "C181Value", 0.3, talas::expr::ValueDomain::Scalar},
      {4, "C181Value", 0.299, talas::expr::ValueDomain::Scalar},
      {6, "C181Value", -0.7, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

talas::runtime::CompileResult
compile(talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry(),
        talas::runtime::RenderSettings settings = makeSettings()) {
  return talas::runtime::compileProject(makeDocument(), std::move(registry),
                                     std::move(settings));
}

const talas::runtime::CompileStep &
thresholdStep(const talas::runtime::PreparedRuntimeGraph &graph,
              std::string_view nodeId = "threshold") {
  const auto found =
      std::ranges::find(graph.plan().steps, nodeId,
                        &talas::runtime::CompileStep::nodeId);
  require(found != graph.plan().steps.end(),
          "Threshold authority fixture should contain its step");
  return *found;
}

talas::runtime::CompileStep &thresholdStep(talas::runtime::RuntimePlan &plan) {
  const auto found =
      std::ranges::find(plan.steps, std::string_view{"threshold"},
                        &talas::runtime::CompileStep::nodeId);
  require(found != plan.steps.end(),
          "Threshold authority forgery should contain its step");
  return *found;
}

talas::runtime::BufferSpec &thresholdOutput(talas::runtime::RuntimePlan &plan) {
  const auto found = std::ranges::find_if(plan.buffers, [](const auto &buffer) {
    return buffer.nodeId == "threshold" && buffer.portId == "value" &&
           buffer.type == talas::core::PortType::Control;
  });
  require(found != plan.buffers.end(),
          "Threshold authority forgery should contain its output");
  return *found;
}

void testExactCapabilityAndCompilerPlan() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.threshold");
  const auto controlSignal =
      talas::core::defaultPortSignalContract(talas::core::PortType::Control);
  const auto exactControlPort = [&](const talas::core::PortDescriptor &port,
                                    std::string_view id, bool required) {
    return port.id == id && port.type == talas::core::PortType::Control &&
           port.required == required && !port.compilerPopulated &&
           port.signal.rate == controlSignal.rate &&
           port.signal.audioLayout == controlSignal.audioLayout &&
           port.signal.edge == controlSignal.edge &&
           port.signal.timelineBearing == controlSignal.timelineBearing &&
           port.signal.spectralFormat == controlSignal.spectralFormat;
  };
  const auto exactExpression = [](const talas::core::ParameterDescriptor &parameter,
                                  std::string_view id,
                                  std::string_view defaultExpression) {
    return parameter.id == id &&
           parameter.kind == talas::core::ParameterKind::Expression &&
           parameter.domain == talas::expr::ValueDomain::Scalar &&
           parameter.defaultExpression == defaultExpression &&
           parameter.defaultChoice.empty() && parameter.choices.empty() &&
           !parameter.defaultBoolean.has_value() &&
           parameter.defaultResource.empty() && parameter.defaultText.empty() &&
           parameter.resourceAuthority ==
               talas::core::ParameterResourceAuthority::None;
  };
  const std::vector<std::string> limitations{
      "offline-and-prepared-matched-checked-finite-absolute-amplitude-schmitt-"
      "latch",
      "static-finite-threshold-and-non-negative-hysteresis-or-per-frame-"
      "connected-controls-with-connected-precedence",
      "exact-upper-inclusive-lower-strict-state-persistence-and-transport-"
      "reset",
      "compiler-owned-exact-descriptor-plan-settings-resource-free-input-"
      "output-producer-scope-latency-and-local-transaction-authority",
      "allocation-free-prepared-callback-failure-atomic-state-output-retry-and-"
      "reset",
      "dynamic-language-direct-scheduled-fallback-edge-event-smoothing-input-"
      "analysis-multichannel-and-exact-reference-semantics-unsupported",
  };
  require(
      descriptor != nullptr && descriptor->displayName == "Threshold" &&
          descriptor->category == "Tracking Live Input" &&
          descriptor->supportLevel == talas::core::NodeSupportLevel::FullRuntime &&
          descriptor->inputs.size() == 3 &&
          exactControlPort(descriptor->inputs[0], "value", true) &&
          exactControlPort(descriptor->inputs[1], "threshold", false) &&
          exactControlPort(descriptor->inputs[2], "hysteresis", false) &&
          descriptor->outputs.size() == 1 &&
          exactControlPort(descriptor->outputs[0], "value", false) &&
          descriptor->parameters.size() == 3 &&
          exactExpression(descriptor->parameters[0], "threshold", "0.5") &&
          exactExpression(descriptor->parameters[1], "hysteresis", "0.0") &&
          descriptor->parameters[2].id == "hysteresisBand" &&
          descriptor->parameters[2].defaultChoice == "full" &&
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
          !descriptor->capabilities.typedEvents &&
          !descriptor->capabilities.transportAware &&
          descriptor->capabilities.latency.kind ==
              talas::core::LatencyKind::None &&
          descriptor->capabilities.resourceAccess ==
              talas::core::ResourceAccessMode::None &&
          descriptor->capabilities.fanInLatencyPolicy ==
              talas::core::FanInLatencyPolicy::NotApplicable &&
          descriptor->capabilities.outputBusLayouts.empty() &&
          descriptor->capabilities.limitations == limitations,
      "exact Threshold descriptor should publish complete Supported authority");

  const auto compiled = compile();
  require(compiled.ok(), "exact Threshold authority graph should compile");
  const auto &step = thresholdStep(*compiled.graph);
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
      "compiler should retain exact promoted Threshold plan authority");
}

void testParityPartitionsAllocationAndReset() {
  const auto compiled = compile();
  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  require(offline.ok() && prepared.ready(),
          "exact Threshold should prepare in both renderers");

  std::array<double, 8> actual{};
  std::size_t offset = 0;
  for (const auto frames : std::array<std::size_t, 4>{1, 2, 3, 2}) {
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered =
        prepared.render(std::span<double>{actual}.subspan(offset, frames));
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "Threshold prepared partitions should render without allocation");
    offset += frames;
  }
  require(std::ranges::equal(actual, offline.mono),
          "Threshold prepared partitions should be bit-identical offline");

  auto resetSettings = makeSettings();
  resetSettings.eventValueChanges.clear();
  resetSettings.eventValues[0].value = 0.7;
  const auto resetGraph =
      compile(talas::core::makeBuiltinNodeRegistry(), std::move(resetSettings));
  talas::runtime::RealtimeRenderSession candidate{*resetGraph.graph};
  talas::runtime::RealtimeRenderSession pristine{*resetGraph.graph};
  std::array<double, 4> scratch{};
  require(candidate.ready() && pristine.ready() && candidate.render(scratch),
          "Threshold reset fixtures should prepare and activate");
  require(candidate.updateEventValue("C181Value", 0.5,
                                     talas::expr::ValueDomain::Scalar) &&
              candidate.render(scratch) && scratch[1] != 0.0,
          "Threshold reset fixture should retain its active latch for an "
          "in-window value");
  candidate.resetTransport();
  std::array<double, 4> resetOutput{};
  std::array<double, 4> pristineOutput{};
  require(candidate.render(resetOutput) && pristine.render(pristineOutput) &&
              resetOutput == pristineOutput,
          "Threshold transport reset should restore the inactive latch");
}

void testDynamicFallbackFollowsItsEventValue() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C181DynamicThreshold", 0.5, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {1, "C181DynamicThreshold", 0.8, talas::expr::ValueDomain::Scalar},
  };
  const auto compiled = talas::runtime::compileProject(
      makeDynamicFallbackDocument(), talas::core::makeBuiltinNodeRegistry(),
      std::move(settings));
  require(compiled.ok(), "dynamic Threshold fallback fixture should compile");
  const auto &dynamic = thresholdStep(*compiled.graph, "dynamicThreshold");
  const auto &staticStep = thresholdStep(*compiled.graph, "staticThreshold");
  require(dynamic.offlineSupport == talas::core::ExecutionSupport::Subset &&
              dynamic.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "dynamic unconnected Threshold fallback must not claim Supported "
          "offline/prepared parity");
  require(staticStep.offlineSupport == talas::core::ExecutionSupport::Supported &&
              staticStep.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "static Threshold in a mixed graph should retain Supported authority");

  const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  std::array<double, 4> preparedOutput{};
  require(offline.ok(),
          "honest Subset Threshold fallback should remain offline-compatible");
  require(prepared.ready(),
          "honest Subset Threshold fallback should prepare: " +
              std::string{prepared.error()});
  require(prepared.render(preparedOutput),
          "honest Subset Threshold fallback should render: " +
              std::string{prepared.error()});
  require(offline.mono[1] == 0.0 &&
              std::ranges::equal(offline.mono, preparedOutput),
          "an Event Value Threshold should follow its change in realtime as "
          "offline");
}

void testCustomCompatibilityAndAuthorityRejection() {
  const auto exact = compile();
  require(exact.ok(), "exact Threshold authority fixture should compile");
  talas::runtime::PreparedRuntimeGraph publicCopy{exact.graph->plan(),
                                               exact.graph->renderSettings()};
  talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
  require(!publicPrepared.ready() &&
              !talas::runtime::renderOffline(publicCopy, 4).ok(),
          "public graph reconstruction must not mint Supported Threshold "
          "authority");

  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport(
      "talas.threshold", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.threshold", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  const auto subset = compile(std::move(subsetRegistry));
  talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
  require(subset.ok() && subsetPrepared.ready() &&
              talas::runtime::renderOffline(*subset.graph, 4).ok(),
          "custom both-Subset Threshold should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.threshold", talas::core::FanInLatencyPolicy::Align);
  const auto divergent = compile(std::move(divergentRegistry));
  talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
  require(divergent.ok() && !divergentPrepared.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
          "divergent descriptor must not mint Supported Threshold authority");

  const auto requireForgeryRejected = [&](std::string_view label,
                                          const auto &mutate) {
    auto forged = *exact.graph;
    auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
    auto &settings =
        const_cast<talas::runtime::RenderSettings &>(forged.renderSettings());
    mutate(plan, settings);
    talas::runtime::RealtimeRenderSession prepared{forged};
    require(!prepared.ready() && !talas::runtime::renderOffline(forged, 4).ok(),
            "forged " + std::string{label} +
                " Threshold authority must fail in both renderers");
  };
  requireForgeryRejected(
      "settings", [](auto &, auto &settings) { settings.maxBlockFrames += 1; });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C181Forged", {0.0}});
  });
  requireForgeryRejected("support", [](auto &plan, auto &) {
    thresholdStep(plan).preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    thresholdStep(plan).preparedRealtimeBinding =
        talas::core::ExecutionBinding::Kernel;
  });
  requireForgeryRejected("reset", [](auto &plan, auto &) {
    thresholdStep(plan).supportsReset = false;
  });
  requireForgeryRejected(
      "scope", [](auto &plan, auto &) { thresholdStep(plan).scopeId = 181; });
  requireForgeryRejected("input producer", [](auto &plan, auto &) {
    thresholdStep(plan).inputs.front().sourceNodeId = "C181ForgedProducer";
  });
  requireForgeryRejected("input port", [](auto &plan, auto &) {
    thresholdStep(plan).inputs.front().inputPortId = "C181ForgedInput";
  });
  requireForgeryRejected("input buffer", [](auto &plan, auto &) {
    thresholdStep(plan).inputs.front().bufferIndex = plan.buffers.size();
  });
  requireForgeryRejected("duplicate input", [](auto &plan, auto &) {
    auto &step = thresholdStep(plan);
    step.inputs.push_back(step.inputs.front());
  });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    thresholdOutput(plan).nodeId = "C181ForgedOwner";
  });
  requireForgeryRejected("output type", [](auto &plan, auto &) {
    thresholdOutput(plan).type = talas::core::PortType::Audio;
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    thresholdOutput(plan).frames -= 1;
  });
  requireForgeryRejected("duplicate output", [](auto &plan, auto &) {
    const auto duplicate = thresholdOutput(plan);
    plan.buffers.push_back(duplicate);
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    thresholdStep(plan).resolvedLatencyFrames = 1;
  });
  requireForgeryRejected("cumulative input latency", [](auto &plan, auto &) {
    thresholdStep(plan).cumulativeInputLatencyFrames = 1;
  });
  requireForgeryRejected("cumulative output latency", [](auto &plan, auto &) {
    thresholdStep(plan).cumulativeOutputLatencyFrames = 1;
  });
  requireForgeryRejected("input latency", [](auto &plan, auto &) {
    thresholdStep(plan).inputLatencyFrames.front() = 0;
  });
  requireForgeryRejected("event value scope", [](auto &plan, auto &) {
    thresholdStep(plan).eventValueScopeId += 1;
  });
  requireForgeryRejected("kernel", [](auto &plan, auto &) {
    thresholdStep(plan).kernelKind = talas::runtime::KernelKind::Logic;
  });
  requireForgeryRejected("isolation", [](auto &plan, auto &) {
    thresholdStep(plan).preparedIsolatedComponent = true;
  });
  requireForgeryRejected("spectral metadata", [](auto &plan, auto &) {
    thresholdStep(plan).preparedSpectralFrameSize = 181;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    thresholdStep(plan).parameters.front().domain =
        talas::expr::ValueDomain::Hertz;
  });
}
} // namespace

int main() {
  testExactCapabilityAndCompilerPlan();
  testParityPartitionsAllocationAndReset();
  testDynamicFallbackFollowsItsEventValue();
  testCustomCompatibilityAndAuthorityRejection();
  return 0;
}
