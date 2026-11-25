#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/KernelRegistry.h"
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

talas::project::ProjectParameterExpression choice(std::string id,
                                               std::string value) {
  talas::project::ProjectParameterExpression parameter;
  parameter.parameterId = std::move(id);
  parameter.choice = std::move(value);
  return parameter;
}

talas::project::ProjectDocument makeDocument(std::string operation = "xor",
                                          std::string left = "!C179Left",
                                          std::string right = "!C179Right",
                                          bool connectLeft = false,
                                          bool connectRight = false) {
  talas::project::ProjectDocument document;
  document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
  document.graph.nodes.push_back({"vca", "talas.vca", {}});
  document.graph.nodes.push_back({
      "oscillator",
      "talas.oscillator",
      {{"frequency", "125 hz"}, {"amplitude", "1"}},
  });
  document.graph.nodes.push_back({
      "logic",
      "talas.logic",
      {choice("operation", std::move(operation)),
       {"left", std::move(left)},
       {"right", std::move(right)}},
  });
  if (connectLeft) {
    document.graph.nodes.push_back({
        "leftControl",
        "talas.linearRamp",
        {{"duration", "0.008 s"}, {"start", "-1"}, {"end", "1"}},
    });
    document.graph.connections.push_back(
        {"leftControl", "value", "logic", "left"});
  }
  if (connectRight) {
    document.graph.nodes.push_back({
        "rightControl",
        "talas.linearRamp",
        {{"duration", "0.008 s"}, {"start", "1"}, {"end", "-1"}},
    });
    document.graph.connections.push_back(
        {"rightControl", "value", "logic", "right"});
  }
  document.graph.connections.push_back({"logic", "gate", "vca", "gain"});
  document.graph.connections.push_back(
      {"oscillator", "audioOut", "vca", "audioIn"});
  document.graph.connections.push_back({"vca", "audioOut", "out", "audioIn"});
  return document;
}

talas::runtime::CompileResult
compile(talas::project::ProjectDocument document,
        talas::runtime::RenderSettings settings = {1000.0, 8},
        talas::core::NodeRegistry registry = talas::core::makeBuiltinNodeRegistry()) {
  return talas::runtime::compileProject(document, std::move(registry),
                                     std::move(settings));
}

void testKernelTruthTablesAndFailureAtomicity() {
  using namespace talas::runtime;
  constexpr std::array left{-1.0, -0.0, 0.0, 0.25};
  constexpr std::array right{1.0, 1.0, -1.0, 0.5};
  constexpr std::array<std::array<double, 4>, 4> expected{{
      {{0.0, 0.0, 0.0, 1.0}},
      {{1.0, 1.0, 0.0, 1.0}},
      {{1.0, 1.0, 0.0, 0.0}},
      {{1.0, 1.0, 1.0, 0.0}},
  }};
  constexpr std::array operations{LogicOperation::And, LogicOperation::Or,
                                  LogicOperation::Xor, LogicOperation::Not};
  for (std::size_t index = 0; index < operations.size(); ++index) {
    LogicKernel kernel;
    std::array<double, 4> output{};
    require(kernel.process({output, KernelSignal::sampleValues(left),
                            KernelSignal::sampleValues(right)}) ==
                KernelProcessStatus::NotPrepared,
            "Logic kernel should reject unprepared use");
    require(kernel.prepare({operations[index]}) == KernelProcessStatus::Ok &&
                kernel.process({output, KernelSignal::sampleValues(left),
                                KernelSignal::sampleValues(right)}) ==
                    KernelProcessStatus::Ok &&
                output == expected[index],
            "Logic kernel should publish exact strict-positive truth tables");
  }

  LogicKernel atomic;
  require(atomic.prepare({LogicOperation::Xor}) == KernelProcessStatus::Ok,
          "atomic Logic kernel should prepare");
  std::array output{179.0, 179.0, 179.0};
  constexpr std::array invalidLeft{1.0, std::numeric_limits<double>::infinity(),
                                   0.0};
  constexpr std::array validRight{0.0, 1.0, 1.0};
  require(atomic.process({output, KernelSignal::sampleValues(invalidLeft),
                          KernelSignal::sampleValues(validRight)}) ==
                  KernelProcessStatus::InvalidValue &&
              output == std::array{179.0, 179.0, 179.0},
          "Logic kernel failure must preserve the complete output span");
  std::array<double, 1> layoutSentinel{179.0};
  require(atomic.process({std::span<double>{layoutSentinel.data(), 0},
                          KernelSignal::constantValue(0.0),
                          KernelSignal::constantValue(0.0)}) ==
                  KernelProcessStatus::InvalidBlockLayout &&
              layoutSentinel[0] == 179.0,
          "Logic kernel should reject zero-frame layouts atomically");
  constexpr std::array<double, 1> shortRight{1.0};
  require(atomic.process({output, KernelSignal::sampleValues(validRight),
                          KernelSignal::sampleValues(shortRight)}) ==
                  KernelProcessStatus::InvalidBlockLayout &&
              output == std::array{179.0, 179.0, 179.0},
          "Logic kernel should reject undersized operands atomically");

  LogicKernel unary;
  require(unary.prepare({LogicOperation::Not}) == KernelProcessStatus::Ok,
          "unary Logic kernel should prepare");
  std::array<double, 3> unaryOutput{};
  require(unary.process({unaryOutput, KernelSignal::sampleValues(validRight),
                         KernelSignal::constantValue(
                             std::numeric_limits<double>::quiet_NaN())}) ==
                  KernelProcessStatus::Ok &&
              unaryOutput == std::array{1.0, 0.0, 0.0},
          "not must ignore the right operand exactly");
}

void testExactCapabilityAndCompilerAuthority() {
  const auto registry = talas::core::makeBuiltinNodeRegistry();
  const auto *descriptor = registry.findNodeType("talas.logic");
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
              descriptor->capabilities.limitations.size() == 6,
          "exact built-in Logic should publish complete supported authority");

  auto settings = talas::runtime::RenderSettings{1000.0, 8};
  settings.eventValues = {
      {"C179Left", -1.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto compiled = compile(makeDocument(), settings);
  require(compiled.ok(), "exact Logic graph should compile");
  const auto logic =
      std::ranges::find(compiled.graph->plan().steps, std::string_view{"logic"},
                        &talas::runtime::CompileStep::nodeId);
  require(logic != compiled.graph->plan().steps.end() &&
              logic->offlineSupport == talas::core::ExecutionSupport::Supported &&
              logic->preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported &&
              logic->kernelKind == talas::runtime::KernelKind::Logic &&
              !logic->supportsReset &&
              logic->cumulativeOutputLatencyFrames == 0,
          "compiler should retain exact promoted Logic authority");
  talas::runtime::RealtimeRenderSession session{*compiled.graph};
  require(session.ready(), std::string{session.error()});
}

void testScheduledParityZeroFrameAndAllocationFreedom() {
  auto preparedSettings = talas::runtime::RenderSettings{1000.0, 8};
  preparedSettings.eventValues = {
      {"C179Left", -1.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  auto offlineSettings = preparedSettings;
  offlineSettings.eventValueChanges = {
      {0, "C179Left", 1.0, talas::expr::ValueDomain::Scalar},
      {2, "C179Right", -1.0, talas::expr::ValueDomain::Scalar},
      {5, "C179Left", 0.0, talas::expr::ValueDomain::Scalar},
      {7, "C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto prepared = compile(makeDocument(), preparedSettings);
  const auto offlineGraph = compile(makeDocument(), offlineSettings);
  require(prepared.ok() && offlineGraph.ok(),
          "scheduled Logic fixtures should compile");
  const auto offline = talas::runtime::renderOffline(*offlineGraph.graph, 10);
  talas::runtime::RealtimeRenderSession session{*prepared.graph};
  require(offline.ok() && session.ready(), "scheduled Logic should prepare");
  for (const auto &change : offlineSettings.eventValueChanges) {
    require(session.scheduleEventValue(change.name, change.value, change.domain,
                                       change.frame),
            "Logic should accept its bounded direct schedule");
  }
  std::array<double, 1> zeroOutput{179.0};
  auto *zeroPointer = zeroOutput.data();
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto zeroRendered = session.render({}, &zeroPointer, 1, 0);
  countAllocations.store(false, std::memory_order_relaxed);
  require(zeroRendered && zeroOutput[0] == 179.0 &&
              allocationCount.load(std::memory_order_relaxed) == 0,
          "Logic zero-frame callback should preserve output, schedule, and "
          "allocation state");

  std::array<double, 10> actual{};
  std::size_t offset = 0;
  for (const auto frames :
       {std::size_t{1}, std::size_t{3}, std::size_t{2}, std::size_t{4}}) {
    auto *output = actual.data() + offset;
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto rendered = session.render({}, &output, 1, frames);
    countAllocations.store(false, std::memory_order_relaxed);
    require(rendered && allocationCount.load(std::memory_order_relaxed) == 0,
            "scheduled Logic callback should be allocation-free");
    offset += frames;
  }
  require(offline.channels.size() == 1 &&
              std::ranges::equal(actual, offline.channels[0]),
          "Logic schedules should be bit-exact across callback partitions");
}

void testSeedingConnectedPrecedenceDemotionAndForgery() {
  auto settings = talas::runtime::RenderSettings{1000.0, 8};
  settings.eventValues = {
      {"C179Left", -1.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  settings.eventValueChanges = {
      {0, "C179Left", 1.0, talas::expr::ValueDomain::Scalar},
      {0, "C179Right", -1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto seeded = compile(makeDocument(), settings);
  require(seeded.ok(), "frame-zero schedules should seed invalid fallbacks" +
                           (seeded.diagnostics.empty()
                                ? std::string{}
                                : ": " + seeded.diagnostics.front().message));
  const auto offline = talas::runtime::renderOffline(*seeded.graph, 8);
  talas::runtime::RealtimeRenderSession seededSession{*seeded.graph};
  std::array<double, 8> actual{};
  auto *actualPointer = actual.data();
  require(offline.ok() && seededSession.ready() &&
              seededSession.render({}, &actualPointer, 1, actual.size()) &&
              std::ranges::equal(actual, offline.channels[0]),
          "frame-zero Logic seeding should retain renderer parity");

  auto connectedSettings = talas::runtime::RenderSettings{1000.0, 8};
  connectedSettings.eventValues = {
      {"C179Left", 0.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 0.0, talas::expr::ValueDomain::Scalar},
  };
  const auto connected =
      compile(makeDocument("xor", "1e200 * 1e200", "1e200 * 1e200", true, true),
              connectedSettings);
  talas::runtime::RealtimeRenderSession connectedSession{*connected.graph};
  require(connected.ok() && connectedSession.ready() &&
              talas::runtime::renderOffline(*connected.graph, 8).ok(),
          "connected inputs should own explicit non-finite fallbacks in both "
          "renderers");
  require(!compile(makeDocument("xor", "1 / 0", "0", true, false),
                   connectedSettings)
               .ok(),
          "connected Logic must not normalize division-by-zero failures");

  const auto contextual = compile(
      makeDocument("xor", "!C179Left + 1", "!C179Right"), connectedSettings);
  require(contextual.ok(), "contextual Logic should retain compatibility");
  const auto contextualLogic = std::ranges::find(
      contextual.graph->plan().steps, std::string_view{"logic"},
      &talas::runtime::CompileStep::nodeId);
  require(contextualLogic != contextual.graph->plan().steps.end() &&
              contextualLogic->offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              contextualLogic->preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset,
          "contextual Logic should demote both axes");

  talas::runtime::RealtimeRenderSession contextualSession{*contextual.graph};
  require(contextualSession.ready() &&
              talas::runtime::renderOffline(*contextual.graph, 8).ok(),
          "contextual both-Subset Logic should retain compatibility");

  const auto stateful =
      compile(makeDocument("xor", "previous(1, 1)", "1"), connectedSettings);
  require(stateful.ok(), "stateful Logic should retain compatibility");
  const auto statefulLogic =
      std::ranges::find(stateful.graph->plan().steps, std::string_view{"logic"},
                        &talas::runtime::CompileStep::nodeId);
  talas::runtime::RealtimeRenderSession statefulSession{*stateful.graph};
  require(statefulLogic != stateful.graph->plan().steps.end() &&
              statefulLogic->offlineSupport ==
                  talas::core::ExecutionSupport::Subset &&
              statefulLogic->preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset &&
              statefulSession.ready() &&
              talas::runtime::renderOffline(*stateful.graph, 8).ok(),
          "stateful Logic should demote both axes without losing "
          "compatibility");

  auto mixedDocument = makeDocument();
  mixedDocument.graph.nodes.push_back({"contextualVca", "talas.vca", {}});
  mixedDocument.graph.nodes.push_back({
      "contextualOscillator",
      "talas.oscillator",
      {{"frequency", "125 hz"}, {"amplitude", "1"}},
  });
  mixedDocument.graph.nodes.push_back({
      "contextualLogic",
      "talas.logic",
      {choice("operation", "xor"),
       {"left", "!C179Left + 1"},
       {"right", "!C179Right"}},
  });
  mixedDocument.graph.connections.push_back(
      {"contextualLogic", "gate", "contextualVca", "gain"});
  mixedDocument.graph.connections.push_back(
      {"contextualOscillator", "audioOut", "contextualVca", "audioIn"});
  mixedDocument.graph.connections.push_back(
      {"contextualVca", "audioOut", "out", "rightIn"});
  const auto mixed = compile(std::move(mixedDocument), connectedSettings);
  require(mixed.ok(), "mixed exact/contextual Logic graph should compile");
  const auto exactMixedLogic =
      std::ranges::find(mixed.graph->plan().steps, std::string_view{"logic"},
                        &talas::runtime::CompileStep::nodeId);
  const auto contextualMixedLogic = std::ranges::find(
      mixed.graph->plan().steps, std::string_view{"contextualLogic"},
      &talas::runtime::CompileStep::nodeId);
  talas::runtime::RealtimeRenderSession mixedSession{*mixed.graph};
  require(exactMixedLogic != mixed.graph->plan().steps.end() &&
              contextualMixedLogic != mixed.graph->plan().steps.end() &&
              exactMixedLogic->preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Supported &&
              contextualMixedLogic->preparedRealtimeSupport ==
                  talas::core::ExecutionSupport::Subset &&
              mixedSession.ready() &&
              talas::runtime::renderOffline(*mixed.graph, 8).ok(),
          "promoted and contextual Logic steps should coexist");

  auto subsetRegistry = talas::core::makeBuiltinNodeRegistry();
  subsetRegistry.setNodeTypeOfflineSupport("talas.logic",
                                           talas::core::ExecutionSupport::Subset,
                                           talas::core::ExecutionBinding::Kernel);
  subsetRegistry.setNodeTypePreparedRealtimeSupport(
      "talas.logic", talas::core::ExecutionSupport::Subset,
      talas::core::ExecutionBinding::Kernel);
  const auto subset =
      compile(makeDocument("xor", "0", "1"), {1000.0, 8}, subsetRegistry);
  require(subset.ok(), "custom both-Subset Logic should compile");
  talas::runtime::RealtimeRenderSession subsetSession{*subset.graph};
  require(subsetSession.ready() &&
              talas::runtime::renderOffline(*subset.graph, 8).ok(),
          "custom both-Subset Logic should retain compatibility");

  auto divergentRegistry = talas::core::makeBuiltinNodeRegistry();
  divergentRegistry.setNodeTypeFanInLatencyPolicy(
      "talas.logic", talas::core::FanInLatencyPolicy::Align);
  const auto divergent =
      compile(makeDocument("xor", "0", "1"), {1000.0, 8}, divergentRegistry);
  require(divergent.ok(), "divergent Logic descriptor should compile");
  talas::runtime::RealtimeRenderSession divergentSession{*divergent.graph};
  require(!divergentSession.ready() &&
              !talas::runtime::renderOffline(*divergent.graph, 8).ok(),
          "divergent descriptor must not mint Supported Logic authority");

  const auto requireForgeryRejected =
      [](const auto &authority, std::string_view label, const auto &mutate) {
        auto forged = authority;
        auto &plan = const_cast<talas::runtime::RuntimePlan &>(forged.plan());
        auto &forgedSettings =
            const_cast<talas::runtime::RenderSettings &>(forged.renderSettings());
        mutate(plan, forgedSettings);
        talas::runtime::RealtimeRenderSession forgedSession{forged};
        require(!forgedSession.ready() &&
                    !talas::runtime::renderOffline(forged, 8).ok(),
                "forged " + std::string{label} +
                    " Logic authority must fail in both renderers");
      };
  requireForgeryRejected(*seeded.graph, "settings", [](auto &, auto &settings) {
    settings.maxBlockFrames += 1;
  });
  requireForgeryRejected(
      *seeded.graph, "resource-free settings", [](auto &, auto &settings) {
        settings.tableResources.push_back({"C179ForgedTable", {0.0}});
      });
  requireForgeryRejected(
      *seeded.graph, "reset lifecycle", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        step.supportsReset = true;
      });
  requireForgeryRejected(
      *seeded.graph, "parameter order", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        std::ranges::swap(step.parameters[1], step.parameters[2]);
      });
  requireForgeryRejected(*seeded.graph, "scope", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                    &talas::runtime::CompileStep::nodeId);
    step.scopeId = 179;
  });
  requireForgeryRejected(*seeded.graph, "latency", [](auto &plan, auto &) {
    auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                    &talas::runtime::CompileStep::nodeId);
    step.resolvedLatencyFrames = 1;
  });
  requireForgeryRejected(
      *seeded.graph, "partial support", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        step.preparedRealtimeSupport = talas::core::ExecutionSupport::Subset;
      });
  requireForgeryRejected(
      *seeded.graph, "output ownership", [](auto &plan, auto &) {
        const auto output =
            std::ranges::find_if(plan.buffers, [](const auto &buffer) {
              return buffer.nodeId == "logic" && buffer.portId == "gate";
            });
        output->nodeId = "C179ForgedOwner";
      });
  requireForgeryRejected(
      *connected.graph, "input producer", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        step.inputs[0].sourceNodeId = "C179ForgedProducer";
      });
  requireForgeryRejected(
      *connected.graph, "input buffer", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        step.inputs[0].bufferIndex = plan.buffers.size();
      });
  requireForgeryRejected(
      *connected.graph, "input order", [](auto &plan, auto &) {
        auto &step = *std::ranges::find(plan.steps, std::string_view{"logic"},
                                        &talas::runtime::CompileStep::nodeId);
        std::ranges::swap(step.inputs[0], step.inputs[1]);
      });
}

void testNotIgnoresRight() {
  auto settings = talas::runtime::RenderSettings{1000.0, 8};
  settings.eventValues = {
      {"C179Left", -1.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto unary =
      compile(makeDocument("not", "!C179Left", "1e200 * 1e200"), settings);
  require(unary.ok() && talas::runtime::renderOffline(*unary.graph, 8).ok(),
          "offline not should ignore right");
  talas::runtime::RealtimeRenderSession session{*unary.graph};
  require(session.ready(),
          "prepared not should ignore right: " + std::string{session.error()});

  auto offlineSettings = settings;
  offlineSettings.eventValueChanges = {
      {4, "C179Right", 2.0, talas::expr::ValueDomain::Scalar},
  };
  const auto lateOverflowOffline = compile(
      makeDocument("not", "!C179Left", "!C179Right * 1e308"), offlineSettings);
  const auto lateOverflowPrepared =
      compile(makeDocument("not", "!C179Left", "!C179Right * 1e308"), settings);
  require(lateOverflowOffline.ok() && lateOverflowPrepared.ok(),
          "late ignored-right overflow fixtures should compile");
  const auto offline =
      talas::runtime::renderOffline(*lateOverflowOffline.graph, 8);
  talas::runtime::RealtimeRenderSession lateSession{*lateOverflowPrepared.graph};
  std::array<double, 8> actual{};
  auto *actualPointer = actual.data();
  require(lateSession.ready() &&
              lateSession.scheduleEventValue(
                  "C179Right", 2.0, talas::expr::ValueDomain::Scalar, 4) &&
              lateSession.render({}, &actualPointer, 1, actual.size()) &&
              offline.ok() && offline.channels.size() == 1 &&
              std::ranges::equal(actual, offline.channels[0]),
          "not should ignore a late non-finite right expression in both "
          "renderers");
}

void testRealtimeLocalAndDownstreamFailureRollback() {
  auto settings = talas::runtime::RenderSettings{1000.0, 8};
  settings.maxRealtimeEventTransactions = 4;
  settings.eventValues = {
      {"C179Left", 0.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto compiled = compile(makeDocument(), settings);
  require(compiled.ok(), "local rollback fixture should compile");
  talas::runtime::RealtimeRenderSession candidate{*compiled.graph};
  talas::runtime::RealtimeRenderSession reference{*compiled.graph};
  require(candidate.ready() && reference.ready() &&
              !candidate.scheduleEventValue(
                  "C179Left", std::numeric_limits<double>::infinity(),
                  talas::expr::ValueDomain::Scalar, 4),
          "typed-event ingress should reject non-finite Logic values");
  std::array<double, 8> actual{};
  std::array<double, 8> expected{};
  auto *actualPointer = actual.data();
  auto *expectedPointer = expected.data();
  allocationCount.store(0, std::memory_order_relaxed);
  countAllocations.store(true, std::memory_order_relaxed);
  const auto rendered = candidate.render({}, &actualPointer, 1, actual.size());
  countAllocations.store(false, std::memory_order_relaxed);
  require(rendered &&
              reference.render({}, &expectedPointer, 1, expected.size()) &&
              actual == expected &&
              allocationCount.load(std::memory_order_relaxed) == 0,
          "rejected non-finite ingress should preserve output and allocate "
          "nothing");

  auto downstreamDocument = makeDocument();
  std::erase_if(downstreamDocument.graph.connections, [](const auto &edge) {
    return edge.fromNode == "logic" && edge.toNode == "vca";
  });
  auto oscillator = std::ranges::find(downstreamDocument.graph.nodes,
                                      std::string_view{"oscillator"},
                                      &talas::project::ProjectNode::instanceId);
  require(oscillator != downstreamDocument.graph.nodes.end(),
          "downstream fixture should contain its oscillator");
  oscillator->parameters[1].expression = "8";
  downstreamDocument.graph.nodes.push_back(
      {"gain", "talas.constant", {{"value", "!C179Gain"}}});
  downstreamDocument.graph.nodes.push_back({"product", "talas.product", {}});
  downstreamDocument.graph.connections.push_back(
      {"logic", "gate", "product", "left"});
  downstreamDocument.graph.connections.push_back(
      {"gain", "value", "product", "right"});
  downstreamDocument.graph.connections.push_back(
      {"product", "value", "vca", "gain"});
  auto downstreamSettings = talas::runtime::RenderSettings{1000.0, 8};
  downstreamSettings.maxRealtimeEventTransactions = 4;
  downstreamSettings.eventValues = {
      {"C179Left", 0.0, talas::expr::ValueDomain::Scalar},
      {"C179Right", 0.0, talas::expr::ValueDomain::Scalar},
      {"C179Gain", std::numeric_limits<double>::max(),
       talas::expr::ValueDomain::Scalar},
  };
  auto offlineSettings = downstreamSettings;
  offlineSettings.eventValueChanges = {
      {4, "C179Left", 1.0, talas::expr::ValueDomain::Scalar},
  };
  const auto offlineCompiled = compile(downstreamDocument, offlineSettings);
  require(offlineCompiled.ok(), "offline downstream fixture should compile");
  talas::runtime::OfflineRenderSession offlineCandidate{*offlineCompiled.graph};
  const auto offlineRejected = offlineCandidate.render(8);
  const auto offlinePrefix = offlineCandidate.render(4);
  const auto offlineRepeated = offlineCandidate.render(4);
  offlineCandidate.reset();
  const auto offlineFresh = offlineCandidate.render(4);
  require(!offlineRejected.ok() && offlineRejected.channels.empty() &&
              offlinePrefix.ok() && !offlineRepeated.ok() &&
              offlineRepeated.channels.empty() && offlineFresh.ok() &&
              offlinePrefix.channels == offlineFresh.channels,
          "downstream rejection should restore offline Logic graph state");

  const auto downstreamCompiled =
      compile(downstreamDocument, downstreamSettings);
  require(downstreamCompiled.ok(),
          "prepared downstream fixture should compile");
  talas::runtime::RealtimeRenderSession downstreamCandidate{
      *downstreamCompiled.graph};
  talas::runtime::RealtimeRenderSession downstreamReference{
      *downstreamCompiled.graph};
  require(downstreamCandidate.ready() && downstreamReference.ready() &&
              downstreamCandidate.scheduleEventValue(
                  "C179Left", 1.0, talas::expr::ValueDomain::Scalar, 4),
          "downstream fixture should schedule a Logic update that enables a "
          "late finite overflow");
  std::array<double, 8> rejected{};
  rejected.fill(179.0);
  auto *rejectedPointer = rejected.data();
  require(
      !downstreamCandidate.render({}, &rejectedPointer, 1, rejected.size()) &&
          rejected == std::array<double, 8>{},
      "downstream failure should publish no Logic graph prefix");
  rejected.fill(179.0);
  rejectedPointer = rejected.data();
  require(
      !downstreamCandidate.render({}, &rejectedPointer, 1, rejected.size()) &&
          rejected == std::array<double, 8>{},
      "repeated downstream failure should restore pending Logic schedules");
  require(downstreamCandidate.updateEventValue("C179Left", 0.0,
                                               talas::expr::ValueDomain::Scalar) &&
              downstreamReference.updateEventValue(
                  "C179Left", 0.0, talas::expr::ValueDomain::Scalar),
          "downstream rollback should accept repair");
  actual.fill(0.0);
  expected.fill(0.0);
  actualPointer = actual.data();
  expectedPointer = expected.data();
  require(downstreamCandidate.render({}, &actualPointer, 1, actual.size()) &&
              downstreamReference.render({}, &expectedPointer, 1,
                                         expected.size()) &&
              actual == expected,
          "downstream repaired retry should restore exact local state");
}
} // namespace

int main() {
  testKernelTruthTablesAndFailureAtomicity();
  testExactCapabilityAndCompilerAuthority();
  testScheduledParityZeroFrameAndAllocationFreedom();
  testSeedingConnectedPrecedenceDemotionAndForgery();
  testNotIgnoresRight();
  testRealtimeLocalAndDownstreamFailureRollback();
  return 0;
}
