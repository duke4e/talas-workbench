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
      {"source",
       "talas.linearRamp",
       {{"start", "0"}, {"end", "1"}, {"duration", "1 s"}}},
      {"quantizer",
       "talas.quantizer",
       {{"stepSize", "!C186Step"}, {"offset", "!C186Offset"}}},
      {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
      {"out", "talas.audioOutput", {}},
  };
  document.graph.connections = {
      {"source", "value", "quantizer", "value"},
      {"quantizer", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
      {"vca", "audioOut", "out", "audioIn"},
  };
  return document;
}

talas::runtime::RenderSettings makeSettings() {
  talas::runtime::RenderSettings settings{8.0, 4};
  settings.eventValues = {
      {"C186Step", 0.25, talas::expr::ValueDomain::Scalar},
      {"C186Offset", 0.0, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {2, "C186Step", 0.5, talas::expr::ValueDomain::Scalar},
      {5, "C186Offset", 0.125, talas::expr::ValueDomain::Scalar},
  };
  return settings;
}

const talas::runtime::CompileStep &
quantizerStep(const talas::runtime::PreparedRuntimeGraph &graph) {
  const auto found =
      std::ranges::find(graph.plan().steps, std::string_view{"talas.quantizer"},
                        &talas::runtime::CompileStep::typeId);
  require(found != graph.plan().steps.end(),
          "Quantizer authority fixture should contain its step");
  return *found;
}

talas::runtime::CompileStep &quantizerStep(talas::runtime::RuntimePlan &plan) {
  const auto found =
      std::ranges::find(plan.steps, std::string_view{"talas.quantizer"},
                        &talas::runtime::CompileStep::typeId);
  require(found != plan.steps.end(),
          "Step Quantizer authority forgery should contain its step");
  return *found;
}

talas::runtime::BufferSpec &quantizerOutput(talas::runtime::RuntimePlan &plan) {
  const auto found = std::ranges::find_if(plan.buffers, [](const auto &buffer) {
    return buffer.nodeId == "quantizer" && buffer.portId == "value" &&
           buffer.type == talas::core::PortType::Control;
  });
  require(found != plan.buffers.end(),
          "Step Quantizer authority forgery should contain its output");
  return *found;
}

void testExactCapabilityAndPrivateCompilerAuthority() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.quantizer");
  const std::vector<std::string> limitations{
      "offline-and-prepared-shared-checked-finite-clean-room-step-quantizer",
      "static-or-direct-event-value-positive-step-size-and-finite-offset",
      "exact-offset-plus-round-value-times-step-size-with-finite-input-and-"
      "result",
      "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-"
      "binding-buffer-producer-scope-zero-latency-and-local-transaction-"
      "authority",
      "allocation-free-prepared-callback-failure-atomic-schedule-output-retry-"
      "and-zero-frame-no-op",
      "alternate-tie-breaking-units-clipping-hysteresis-stochastic-"
      "multichannel-and-exact-reference-semantics-unsupported",
  };
  require(descriptor != nullptr &&
              descriptor->capabilities.offline ==
                  talas::core::ExecutionSupport::Supported &&
              descriptor->capabilities.offlineBinding ==
                  talas::core::ExecutionBinding::Kernel &&
              descriptor->capabilities.preparedRealtime ==
                  talas::core::ExecutionSupport::Supported &&
              descriptor->capabilities.preparedRealtimeBinding ==
                  talas::core::ExecutionBinding::Kernel &&
              descriptor->capabilities.state ==
                  talas::core::NodeStateModel::Stateless &&
              !descriptor->capabilities.supportsReset &&
              !descriptor->capabilities.typedEvents &&
              !descriptor->capabilities.transportAware &&
              descriptor->capabilities.latency.kind ==
                  talas::core::LatencyKind::None &&
              descriptor->capabilities.resourceAccess ==
                  talas::core::ResourceAccessMode::None &&
              descriptor->capabilities.fanInLatencyPolicy ==
                  talas::core::FanInLatencyPolicy::NotApplicable &&
              descriptor->capabilities.limitations == limitations,
          "exact Step Quantizer descriptor should publish Supported authority");

  const auto compiled =
      talas::runtime::compileProject(makeDocument(), registry, makeSettings());
  require(compiled.ok() &&
              quantizerStep(*compiled.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              quantizerStep(*compiled.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "compiler should retain exact Step Quantizer Supported authority");

  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession prepared{*compiled.graph};
  require(offline.ok() && prepared.ready(),
          "exact Step Quantizer should prepare in both renderers");
  std::array<double, 1> zeroOutput{186.0};
  auto *zeroPointer = zeroOutput.data();
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto zeroRendered = prepared.render({}, &zeroPointer, 1, 0);
  countAllocations.store(false, std::memory_order_relaxed);
  require(
      zeroRendered && zeroOutput[0] == 186.0 &&
          allocationCount.load(std::memory_order_relaxed) == 0,
      "Step Quantizer zero-frame callback should be an allocation-free no-op");
  std::array<double, 8> actual{};
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto first = prepared.render(std::span<double>{actual}.first(3));
  const auto second = prepared.render(std::span<double>{actual}.subspan(3, 3));
  const auto third = prepared.render(std::span<double>{actual}.subspan(6));
  countAllocations.store(false, std::memory_order_relaxed);
  require(first && second && third &&
              allocationCount.load(std::memory_order_relaxed) == 0 &&
              std::ranges::equal(actual, offline.mono),
          "exact Step Quantizer should preserve irregular offline/prepared "
          "parity without callback allocation");

  talas::runtime::PreparedRuntimeGraph publicCopy{
      compiled.graph->plan(), compiled.graph->renderSettings()};
  talas::runtime::RealtimeRenderSession publicPrepared{publicCopy};
  require(!publicPrepared.ready() &&
              !talas::runtime::renderOffline(publicCopy, 4).ok(),
          "public graph reconstruction must not mint Supported Quantizer "
          "authority");
}

void testDynamicFallbackIsHonestSubset() {
  const auto requireDemotion = [](std::string expression,
                                  std::string_view parameterId,
                                  std::string_view label) {
    auto document = makeDocument();
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    auto node =
        std::ranges::find(document.graph.nodes, std::string_view{"quantizer"},
                          &talas::project::ProjectNode::instanceId);
    require(node != document.graph.nodes.end(),
            "dynamic Step Quantizer fixture should contain its node");
    auto parameter = std::ranges::find(
        node->parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    require(parameter != node->parameters.end(),
            "dynamic Step Quantizer fixture should contain its parameter");
    parameter->expression = std::move(expression);
    const auto result = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), makeSettings());
    require(result.ok() &&
                quantizerStep(*result.graph).offlineSupport ==
                    talas::core::ExecutionSupport::Subset &&
                quantizerStep(*result.graph).preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Subset,
            std::string{label} +
                " Step Quantizer parameter must honestly downgrade both axes");
    talas::runtime::RealtimeRenderSession prepared{*result.graph};
    require(prepared.ready() &&
                talas::runtime::renderOffline(*result.graph, 4).ok(),
            std::string{label} +
                " both-Subset Step Quantizer should retain compatibility");
  };
  requireDemotion("!C186Step + 0", "stepSize", "computed Event Value");
  requireDemotion("previous(0.25, 0.25)", "stepSize", "stateful");
  requireDemotion("[pi, 0.25][1]", "stepSize", "identifier-bearing");
  requireDemotion("!C186Offset + 0", "offset",
                  "computed Event Value offset");
  requireDemotion("previous(0, 0)", "offset", "stateful offset");

  auto variableDocument = makeDocument();
  variableDocument.expressionLanguage =
      talas::expr::ExpressionLanguageVersion::VariablesV2;
  variableDocument.variableLibrary.variables.push_back({
      "step",
      "Step",
      talas::expr::ValueDomain::Scalar,
      0.25,
      {talas::project::ProjectVariableOwnerKind::Project, {}},
      true,
  });
  auto node = std::ranges::find(variableDocument.graph.nodes,
                                std::string_view{"quantizer"},
                                &talas::project::ProjectNode::instanceId);
  node->parameters[0].expression = "@step";
  const auto variable = talas::runtime::compileProject(
      variableDocument, talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(variable.ok() &&
              quantizerStep(*variable.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              quantizerStep(*variable.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "variable-backed Step Quantizer must honestly downgrade both axes");

  auto variableOffsetDocument = makeDocument();
  variableOffsetDocument.expressionLanguage =
      talas::expr::ExpressionLanguageVersion::VariablesV2;
  variableOffsetDocument.variableLibrary.variables.push_back({
      "offset",
      "Offset",
      talas::expr::ValueDomain::Scalar,
      0.0,
      {talas::project::ProjectVariableOwnerKind::Project, {}},
      true,
  });
  auto offsetNode = std::ranges::find(
      variableOffsetDocument.graph.nodes, std::string_view{"quantizer"},
      &talas::project::ProjectNode::instanceId);
  offsetNode->parameters[1].expression = "@offset";
  const auto variableOffset = talas::runtime::compileProject(
      variableOffsetDocument, talas::core::makeBuiltinNodeRegistry(),
      makeSettings());
  require(variableOffset.ok() &&
              quantizerStep(*variableOffset.graph).offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              quantizerStep(*variableOffset.graph).preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "variable-backed Step Quantizer offset must honestly downgrade both axes");
}

void testNestedAndMidiVoiceScopeAuthority() {
  talas::project::ProjectSoundDefinition nestedDefinition;
  nestedDefinition.definitionId = "C186NestedQuantizer";
  nestedDefinition.displayName = "C186 Nested Step Quantizer";
  nestedDefinition.outputs.push_back(
      {"audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
  nestedDefinition.graph.nodes = {
      {"source",
       "talas.linearRamp",
       {{"start", "0"}, {"end", "1"}, {"duration", "1 s"}}},
      {"quantizer", "talas.quantizer", {{"stepSize", "0.25"}, {"offset", "0"}}},
      {"carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
  };
  nestedDefinition.graph.connections = {
      {"source", "value", "quantizer", "value"},
      {"quantizer", "value", "vca", "gain"},
      {"carrier", "audioOut", "vca", "audioIn"},
  };
  talas::project::ProjectDocument nestedDocument;
  nestedDocument.soundDefinitions.push_back(std::move(nestedDefinition));
  nestedDocument.graph.nodes = {
      soundInstance("nested", "C186NestedQuantizer"),
      {"out", "talas.audioOutput", {}},
  };
  nestedDocument.graph.connections = {
      {"nested", "audioOut", "out", "audioIn"},
  };
  const talas::runtime::RenderSettings nestedSettings{8.0, 8};
  const auto nested = talas::runtime::compileProject(
      nestedDocument, talas::core::makeBuiltinNodeRegistry(), nestedSettings);
  require(nested.ok(), "nested Step Quantizer authority graph should compile");
  const auto &nestedQuantizer = quantizerStep(*nested.graph);
  require(nestedQuantizer.scopeId != talas::runtime::globalRuntimeScope &&
              nestedQuantizer.structuralScopeId == nestedQuantizer.scopeId &&
              nestedQuantizer.eventValueScopeId == nestedQuantizer.scopeId &&
              nestedQuantizer.offlineSupport ==
                  talas::core::ExecutionSupport::Supported &&
              nestedQuantizer.preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported,
          "nested Step Quantizer should retain Supported authority in its "
          "structural scope");
  const auto nestedOffline = talas::runtime::renderOffline(*nested.graph, 8);
  talas::runtime::RealtimeRenderSession nestedPrepared{*nested.graph};
  std::array<double, 8> nestedActual{};
  require(nestedOffline.ok() && nestedPrepared.ready() &&
              nestedPrepared.render(nestedActual) &&
              std::ranges::equal(nestedActual, nestedOffline.mono),
          "nested Step Quantizer should preserve offline/prepared parity");

  talas::project::ProjectDocument midiDocument;
  midiDocument.graph.nodes = {
      {"source", "talas.constant", {{"value", "0.5"}}},
      {"quantizer", "talas.quantizer", {{"stepSize", "0.25"}, {"offset", "0"}}},
      {"carrier",
       "talas.oscillator",
       {{"frequency", "440 hz"}, {"amplitude", "1"}}},
      {"vca", "talas.vca", {}},
      {"midi",
       "talas.midiVoice",
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
      {"source", "value", "quantizer", "value"},
      {"quantizer", "value", "vca", "gain"},
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
          "MIDI Voice Step Quantizer graph should compile two voices");
  const auto &midiRegion = midi.graph->plan().midiVoices.front();
  for (const auto &voice : midiRegion.voices) {
    const auto first = midi.graph->plan().steps.begin() +
                       static_cast<std::ptrdiff_t>(voice.firstStepIndex);
    const auto last = first + static_cast<std::ptrdiff_t>(voice.stepCount);
    const auto quantizer =
        std::ranges::find(first, last, std::string_view{"talas.quantizer"},
                          &talas::runtime::CompileStep::typeId);
    require(quantizer != last && quantizer->scopeId == voice.scopeId &&
                quantizer->structuralScopeId ==
                    talas::runtime::globalRuntimeScope &&
                quantizer->eventValueScopeId == quantizer->structuralScopeId &&
                quantizer->offlineSupport ==
                    talas::core::ExecutionSupport::Supported &&
                quantizer->preparedRealtimeSupport ==
                    talas::core::ExecutionSupport::Supported,
            "each MIDI Voice clone should retain exact Supported Step "
            "Quantizer authority");
  }
  const auto midiOffline = talas::runtime::renderOffline(*midi.graph, 16);
  talas::runtime::RealtimeRenderSession midiPrepared{*midi.graph};
  std::array<double, 16> midiActual{};
  require(midiOffline.ok(), "offline MIDI Voice Step Quantizer should render");
  require(midiPrepared.ready(),
          "prepared MIDI Voice Step Quantizer should prepare: " +
              std::string{midiPrepared.error()});
  require(midiPrepared.render(midiActual) &&
              std::ranges::equal(midiActual, midiOffline.mono),
          "MIDI Voice Step Quantizer clones should preserve parity");
}

void testCustomCompatibilityAndAuthorityRejection() {
  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport("talas.quantizer",
                                           talas::core::ExecutionSupport::Subset,
                                           talas::core::ExecutionBinding::Kernel);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.quantizer", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::Kernel);
  const auto subset = talas::runtime::compileProject(
      makeDocument(), std::move(subsetRegistry), makeSettings());
  require(subset.ok(), "custom both-Subset Step Quantizer should compile");
  talas::runtime::RealtimeRenderSession subsetPrepared{*subset.graph};
  require(subsetPrepared.ready() &&
              talas::runtime::renderOffline(*subset.graph, 4).ok(),
          "custom both-Subset Step Quantizer should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.quantizer", talas::core::FanInLatencyPolicy::Align);
  const auto divergent = talas::runtime::compileProject(
      makeDocument(), std::move(divergentRegistry), makeSettings());
  require(divergent.ok(), "divergent Step Quantizer should still compile");
  talas::runtime::RealtimeRenderSession divergentPrepared{*divergent.graph};
  require(
      !divergentPrepared.ready() &&
          !talas::runtime::renderOffline(*divergent.graph, 4).ok(),
      "divergent descriptor must not mint Supported Step Quantizer authority");

  const auto exact = talas::runtime::compileProject(
      makeDocument(), talas::core::makeBuiltinNodeRegistry(), makeSettings());
  require(exact.ok(), "exact Step Quantizer forgery fixture should compile");
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
                " Step Quantizer authority must fail in both renderers");
  };
  requireForgeryRejected(
      "settings", [](auto &, auto &settings) { settings.maxBlockFrames += 1; });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C186Forged", {0.0}});
  });
  requireForgeryRejected("support", [](auto &plan, auto &) {
    quantizerStep(plan).preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    quantizerStep(plan).preparedRealtimeBinding =
        talas::core::ExecutionBinding::LegacyAdapter;
  });
  requireForgeryRejected("kernel", [](auto &plan, auto &) {
    quantizerStep(plan).kernelKind = talas::runtime::KernelKind::Gate;
  });
  requireForgeryRejected("reset", [](auto &plan, auto &) {
    quantizerStep(plan).supportsReset = true;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    quantizerStep(plan).parameters[0].domain = talas::expr::ValueDomain::Hertz;
  });
  requireForgeryRejected("input producer", [](auto &plan, auto &) {
    quantizerStep(plan).inputs[0].sourceNodeId = "C186ForgedProducer";
  });
  requireForgeryRejected("source buffer", [](auto &plan, auto &) {
    quantizerStep(plan).inputs[0].bufferIndex = plan.buffers.size();
  });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    quantizerOutput(plan).nodeId = "C186ForgedOwner";
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    quantizerOutput(plan).frames -= 1;
  });
  requireForgeryRejected(
      "scope", [](auto &plan, auto &) { quantizerStep(plan).scopeId = 186; });
  requireForgeryRejected("event scope", [](auto &plan, auto &) {
    quantizerStep(plan).eventValueScopeId += 1;
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    quantizerStep(plan).resolvedLatencyFrames = 1;
  });
}
} // namespace

int main() {
  testExactCapabilityAndPrivateCompilerAuthority();
  testDynamicFallbackIsHonestSubset();
  testNestedAndMidiVoiceScopeAuthority();
  testCustomCompatibilityAndAuthorityRejection();
  return 0;
}
