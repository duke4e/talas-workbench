#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
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

talas::project::ProjectDocument makeDocument() {
  talas::project::ProjectDocument document;
  document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
  document.graph.nodes.push_back({"annotation", "talas.annotation", {}});
  document.graph.nodes.push_back({
      "input",
      "talas.audioInput",
      {booleanParameter("channel1", true), booleanParameter("channel2", false)},
  });
  document.graph.connections.push_back(
      {"input", "audioOut", "annotation", "audioIn"});
  document.graph.connections.push_back(
      {"annotation", "audioOut", "out", "audioIn"});
  return document;
}

talas::project::ProjectDocument makeDownstreamFailureDocument() {
  auto document = makeDocument();
  document.graph.nodes.insert(document.graph.nodes.begin() + 1,
                              {"vca", "talas.vca", {}});
  document.graph.nodes.insert(
      document.graph.nodes.begin() + 2,
      {"gain", "talas.constant", {{"value", "!C180Gain"}}});
  document.graph.connections.pop_back();
  document.graph.connections.push_back(
      {"annotation", "audioOut", "vca", "audioIn"});
  document.graph.connections.push_back({"gain", "value", "vca", "gain"});
  document.graph.connections.push_back({"vca", "audioOut", "out", "audioIn"});
  return document;
}

talas::project::ProjectDocument makeBypassDocument() {
  auto document = makeDocument();
  document.graph.nodes.erase(document.graph.nodes.begin() + 1);
  document.graph.connections.clear();
  document.graph.connections.push_back({"input", "audioOut", "out", "audioIn"});
  return document;
}

talas::project::ProjectDocument makeAlternateSourcePortDocument() {
  talas::project::ProjectDocument document;
  document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
  document.graph.nodes.push_back({"annotation", "talas.annotation", {}});
  document.graph.nodes.push_back({"pan", "talas.pan", {}});
  document.graph.nodes.push_back({
      "oscillator",
      "talas.oscillator",
      {{"frequency", "750 hz"}, {"amplitude", "0.5"}},
  });
  document.graph.connections.push_back(
      {"oscillator", "audioOut", "pan", "audioIn"});
  document.graph.connections.push_back(
      {"pan", "leftOut", "annotation", "audioIn"});
  document.graph.connections.push_back(
      {"annotation", "audioOut", "out", "audioIn"});
  document.graph.connections.push_back({"pan", "rightOut", "out", "rightIn"});
  return document;
}

talas::runtime::CompileResult
compile(talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry()) {
  talas::runtime::RenderSettings settings{48000.0, 8};
  settings.audioInputChannels = {
      {1, {1.0, -0.5, 0.25, -0.0, 1.0, -1.0, 0.125, -0.125}},
  };
  return talas::runtime::compileProject(makeDocument(), std::move(registry),
                                     std::move(settings));
}

talas::runtime::CompileResult compileWithSettings(
    talas::project::ProjectDocument document, talas::runtime::RenderSettings settings,
    talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry()) {
  return talas::runtime::compileProject(std::move(document), std::move(registry),
                                     std::move(settings));
}

void testExactCapabilityAndPassThroughParity() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.annotation");
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
                  talas::core::NodeStateModel::Stateless &&
              !descriptor->capabilities.supportsReset &&
              descriptor->capabilities.latency.kind ==
                  talas::core::LatencyKind::None &&
              descriptor->capabilities.outputBusLayouts ==
                  std::vector{talas::core::AudioBusLayoutKind::Mono} &&
              descriptor->capabilities.limitations.size() == 5,
          "exact Annotation should publish complete Supported authority");

  const auto compiled = compile();
  require(compiled.ok(), "exact Annotation graph should compile");
  const auto annotation = std::ranges::find(compiled.graph->plan().steps,
                                            std::string_view{"annotation"},
                                            &talas::runtime::CompileStep::nodeId);
  require(
      annotation != compiled.graph->plan().steps.end() &&
          annotation->offlineSupport == talas::core::ExecutionSupport::Supported &&
          annotation->preparedRealtimeSupport ==
              talas::core::ExecutionSupport::Supported &&
          !annotation->kernelKind.has_value() && !annotation->supportsReset &&
          annotation->resolvedLatencyFrames == 0 &&
          annotation->cumulativeOutputLatencyFrames == 0,
      "compiler should retain promoted Annotation authority");

  const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
  talas::runtime::RealtimeRenderSession session{*compiled.graph};
  require(offline.ok() && session.ready(),
          "exact Annotation should prepare in both renderers");

  constexpr std::array<float, 8> input{1.0F, -0.5F, 0.25F,  -0.0F,
                                       1.0F, -1.0F, 0.125F, -0.125F};
  const std::array<const float *, 1> channels{input.data()};
  std::array<double, 8> actual{};
  std::size_t offset = 0;
  for (const auto frameCount :
       {std::size_t{1}, std::size_t{3}, std::size_t{4}}) {
    auto *output = actual.data() + offset;
    const auto *inputPointer = input.data() + offset;
    const std::array<const float *, 1> blockChannels{inputPointer};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render(
        {blockChannels.data(), blockChannels.size()}, &output, 1, frameCount);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "Annotation callbacks should be allocation-free");
    offset += frameCount;
  }
  require(std::ranges::equal(actual, offline.channels[0]),
          "Annotation should match offline across callback partitions");

  std::array<double, 1> sentinel{180.0};
  auto *sentinelPointer = sentinel.data();
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto zeroRendered = session.render({channels.data(), channels.size()},
                                           &sentinelPointer, 1, 0);
  countAllocations.store(false, std::memory_order_relaxed);
  require(zeroRendered && sentinel[0] == 180.0 &&
              allocationCount.load(std::memory_order_relaxed) == 0,
          "Annotation zero-frame callback should preserve caller output");

  const auto alternate =
      compileWithSettings(makeAlternateSourcePortDocument(), {48000.0, 8});
  talas::runtime::RealtimeRenderSession alternateSession{*alternate.graph};
  require(alternate.ok() && alternateSession.ready() &&
              talas::runtime::renderOffline(*alternate.graph, 8).ok(),
          "Annotation should accept an authoritative non-audioOut Audio "
          "producer port");

  auto captureSettings = talas::runtime::RenderSettings{48000.0, 8};
  captureSettings.enableRealtimeAnalysisCapture = true;
  captureSettings.audioInputChannels = {{1, std::vector<double>(8, 0.25)}};
  const auto capture = compileWithSettings(makeDocument(), captureSettings);
  require(capture.ok() && capture.graph->analysisCapture() == nullptr,
          "Annotation should not create an analysis capture");
}

void testCustomCompatibilityAndAuthorityRejection() {
  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport(
      "talas.annotation", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.annotation", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::LegacyAdapter);
  const auto subset = compile(std::move(subsetRegistry));
  talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
  require(subset.ok() && subsetSession.ready() &&
              talas::runtime::renderOffline(*subset.graph, 8).ok(),
          "custom both-Subset Annotation should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.annotation", talas::core::FanInLatencyPolicy::Align);
  const auto divergent = compile(std::move(divergentRegistry));
  talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
  require(divergent.ok() && !divergentSession.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 8).ok(),
          "divergent descriptor must not mint Supported Annotation authority");

  const auto authority = compile();
  const auto requireForgeryRejected = [&](std::string_view label,
                                          const auto &mutate) {
    auto forged = *authority.graph;
    auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
    auto &settings =
        const_cast<talas::runtime::RenderSettings &>(forged.renderSettings());
    mutate(plan, settings);
    talas::runtime::RealtimeRenderSession session{forged};
    require(!session.ready() && !talas::runtime::renderOffline(forged, 8).ok(),
            "forged " + std::string{label} +
                " Annotation authority must fail in both renderers");
  };
  requireForgeryRejected(
      "settings", [](auto &, auto &settings) { settings.maxBlockFrames += 1; });
  requireForgeryRejected("resource", [](auto &, auto &settings) {
    settings.tableResources.push_back({"C180Forged", {0.0}});
  });
  requireForgeryRejected("input producer", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.inputs[0].sourceNodeId = "C180ForgedProducer";
  });
  requireForgeryRejected("input port", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.inputs[0].inputPortId = "forgedAudioIn";
  });
  requireForgeryRejected("input buffer", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.inputs[0].bufferIndex = plan.buffers.size();
  });
  requireForgeryRejected("output owner", [](auto &plan, auto &) {
    const auto output =
        std::ranges::find_if(plan.buffers, [](const auto &buffer) {
          return buffer.nodeId == "annotation" && buffer.portId == "audioOut";
        });
    output->nodeId = "C180ForgedOwner";
  });
  requireForgeryRejected("scope", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.scopeId = 180;
  });
  requireForgeryRejected("parameter", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.parameters.push_back({"forged", talas::expr::ValueDomain::Scalar, 0.0});
  });
  requireForgeryRejected("reset lifecycle", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.supportsReset = true;
  });
  requireForgeryRejected("partial support", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
  });
  requireForgeryRejected("binding", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.preparedRealtimeBinding = talas::core::ExecutionBinding::Kernel;
  });
  requireForgeryRejected("output type", [](auto &plan, auto &) {
    const auto output =
        std::ranges::find_if(plan.buffers, [](const auto &buffer) {
          return buffer.nodeId == "annotation" && buffer.portId == "audioOut";
        });
    output->type = talas::core::PortType::Control;
  });
  requireForgeryRejected("output capacity", [](auto &plan, auto &) {
    const auto output =
        std::ranges::find_if(plan.buffers, [](const auto &buffer) {
          return buffer.nodeId == "annotation" && buffer.portId == "audioOut";
        });
    output->frames -= 1;
  });
  requireForgeryRejected("latency", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"annotation"},
                                    &talas::runtime::CompileStep::nodeId);
    step.resolvedLatencyFrames = 1;
  });
}

void testBoundaryTransparencyAndDownstreamRollback() {
  talas::runtime::RenderSettings transparentSettings{48000.0, 8};
  transparentSettings.audioInputChannels = {
      {1,
       {-0.0, std::numeric_limits<double>::denorm_min(),
        std::numeric_limits<double>::max(),
        std::numeric_limits<double>::lowest(), 1.0, -1.0, 0.5, -0.5}},
  };
  const auto transparent =
      compileWithSettings(makeDocument(), transparentSettings);
  const auto bypass =
      compileWithSettings(makeBypassDocument(), transparentSettings);
  require(transparent.ok() && bypass.ok(),
          "boundary Annotation and bypass graphs should compile");
  const auto offline = talas::runtime::renderOffline(*transparent.graph, 8);
  const auto bypassOffline = talas::runtime::renderOffline(*bypass.graph, 8);
  require(offline.ok() && bypassOffline.ok() &&
              offline.mono.size() == bypassOffline.mono.size(),
          "boundary Annotation and bypass references should render");
  for (std::size_t frame = 0; frame < offline.mono.size(); ++frame) {
    require(std::bit_cast<std::uint64_t>(offline.mono[frame]) ==
                std::bit_cast<std::uint64_t>(bypassOffline.mono[frame]),
            "Annotation should preserve its producer sample bits");
  }

  talas::runtime::RealtimeRenderSession preparedAnnotation{*transparent.graph};
  talas::runtime::RealtimeRenderSession preparedBypass{*bypass.graph};
  require(preparedAnnotation.ready() && preparedBypass.ready(),
          "prepared Annotation and bypass bit oracles should be ready");
  constexpr std::array<float, 8> preparedInput{
      -0.0F,
      std::numeric_limits<float>::denorm_min(),
      std::numeric_limits<float>::max(),
      std::numeric_limits<float>::lowest(),
      1.0F,
      -1.0F,
      0.5F,
      -0.5F,
  };
  const std::array<const float *, 1> preparedChannels{preparedInput.data()};
  std::array<double, 8> preparedAnnotationOutput{};
  std::array<double, 8> preparedBypassOutput{};
  auto *preparedAnnotationPointer = preparedAnnotationOutput.data();
  auto *preparedBypassPointer = preparedBypassOutput.data();
  require(preparedAnnotation.render(
              {preparedChannels.data(), preparedChannels.size()},
              &preparedAnnotationPointer, 1, preparedInput.size()) &&
              preparedBypass.render(
                  {preparedChannels.data(), preparedChannels.size()},
                  &preparedBypassPointer, 1, preparedInput.size()),
          "prepared Annotation and bypass bit oracles should render");
  for (std::size_t frame = 0; frame < preparedInput.size(); ++frame) {
    require(std::bit_cast<std::uint64_t>(preparedAnnotationOutput[frame]) ==
                std::bit_cast<std::uint64_t>(preparedBypassOutput[frame]),
            "prepared Annotation should preserve its producer sample bits");
  }

  talas::runtime::RenderSettings failureSettings{48000.0, 4};
  failureSettings.eventValues = {
      {"C180Gain", 1.0e308, talas::expr::ValueDomain::Scalar},
  };
  failureSettings.audioInputChannels = {{1, {2.0, 2.0, 2.0, 2.0}}};
  const auto failure =
      compileWithSettings(makeDownstreamFailureDocument(), failureSettings);
  require(failure.ok(), "downstream Annotation rollback graph should compile");
  talas::runtime::RealtimeRenderSession session{*failure.graph};
  require(session.ready(), std::string{session.error()});
  constexpr std::array<float, 4> input{2.0F, 2.0F, 2.0F, 2.0F};
  const std::array<const float *, 1> channels{input.data()};
  std::array<double, 4> output{180.0, 180.0, 180.0, 180.0};
  auto *outputPointer = output.data();
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto first = session.render({channels.data(), channels.size()},
                                    &outputPointer, 1, output.size());
  const auto second = session.render({channels.data(), channels.size()},
                                     &outputPointer, 1, output.size());
  countAllocations.store(false, std::memory_order_relaxed);
  require(!first && !second,
          "downstream overflow should reject every attempted callback");
  require(output == std::array<double, 4>{},
          "downstream overflow must not publish an Annotation prefix");
  require(allocationCount.load(std::memory_order_relaxed) == 0,
          "downstream overflow rejection should not allocate");
  require(
      session.updateEventValue("C180Gain", 1.0, talas::expr::ValueDomain::Scalar),
      "failed callback should preserve repair authority");
  const auto repaired = session.render({channels.data(), channels.size()},
                                       &outputPointer, 1, output.size());
  require(repaired, "repaired retry should render");
  require(output == std::array{2.0, 2.0, 2.0, 2.0},
          "repaired retry should publish the exact Annotation signal");
}
} // namespace

int main() {
  testExactCapabilityAndPassThroughParity();
  testCustomCompatibilityAndAuthorityRejection();
  testBoundaryTransparencyAndDownstreamRollback();
  return 0;
}
