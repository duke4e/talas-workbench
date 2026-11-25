#include "talas/core/BuiltinNodes.h"
#include "talas/dsp/ScaleVocoderIntervals.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <pthread.h>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using talas::runtime::DiagnosticCode;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, double tolerance, const std::string& message)
{
    const auto difference = actual > expected ? actual - expected : expected - actual;
    if (difference > tolerance)
        throw std::runtime_error(message + ": expected " + std::to_string(expected) + ", got " + std::to_string(actual));
}

talas::core::NodeRegistry makeRegistry()
{
    return talas::core::makeBuiltinNodeRegistry();
}

talas::project::ProjectNode node(std::string id, std::string typeId, std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(typeId), std::move(parameters)};
}

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string choice)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndChoiceParameter(std::string id, std::string expression, std::string choice)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.choice = std::move(choice);
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndBooleanParameter(std::string id, std::string expression, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndResourceParameter(std::string id, std::string expression, std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectParameterExpression booleanAndTextParameter(std::string id, bool value, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectParameterExpression choiceAndBooleanParameter(std::string id, std::string choice, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    parameter.boolean = value;
    return parameter;
}

talas::core::Connection connection(std::string fromNode, std::string fromPort, std::string toNode, std::string toPort)
{
    return {std::move(fromNode), std::move(fromPort), std::move(toNode), std::move(toPort)};
}

talas::project::ProjectDocument makeBasicAudioDocument(std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = {},
                                                    std::vector<talas::project::ProjectParameterExpression> vcaParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca", std::move(vcaParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeMissingComponentAudioDocument()
{
    auto document = makeBasicAudioDocument();
    auto& missing = document.graph.nodes[1];
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    missing.parameters.clear();
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    const auto monoAudio = talas::core::PortSignalContract{
        talas::core::PortSignalRate::Sample,
        talas::core::AudioBusLayoutKind::Mono,
        talas::core::PortEdgeSemantics::Continuous,
        talas::core::SpectralFrameFormat::NotApplicable,
        true,
    };
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        monoAudio,
    });
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        monoAudio,
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    missing.preservedComponent = std::move(component);
    document.graph.connections = {
        connection("osc", "audioOut", "missing1", "audioIn"),
        connection("missing1", "audioOut", "out", "audioIn"),
    };
    return document;
}

talas::project::ProjectDocument makeNestedMissingComponentAudioDocument()
{
    auto source = makeMissingComponentAudioDocument();
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "missing1",
        "audioOut",
    });
    definition.graph.nodes = {
        std::move(source.graph.nodes[1]),
        std::move(source.graph.nodes[2]),
    };
    definition.graph.connections = {
        {"osc", "audioOut", "missing1", "audioIn"},
    };
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    document.graph.nodes = {
        std::move(instance),
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"voice1", "audioOut", "out", "audioIn"},
    };
    return document;
}

// Compiles run on Workbench worker threads, whose stacks are 512 KiB.
template <typename Function>
void runOnSmallStack(Function function, std::size_t stackBytes)
{
    struct Context
    {
        Function* function = nullptr;
        std::exception_ptr failure;
    } context{&function, {}};
    pthread_attr_t attributes;
    require(pthread_attr_init(&attributes) == 0
            && pthread_attr_setstacksize(&attributes, stackBytes) == 0,
        "small-stack thread attributes should apply");
    pthread_t thread;
    const auto started = pthread_create(
        &thread,
        &attributes,
        [](void* raw) -> void* {
            auto& state = *static_cast<Context*>(raw);
            try
            {
                (*state.function)();
            }
            catch (...)
            {
                state.failure = std::current_exception();
            }
            return nullptr;
        },
        &context);
    pthread_attr_destroy(&attributes);
    require(started == 0, "small-stack thread should start");
    pthread_join(thread, nullptr);
    if (context.failure)
        std::rethrow_exception(context.failure);
}

void testLongChainsCompileOnAWorkerStack()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}});
    std::string previous = "osc";
    for (std::size_t index = 0; index < 8000; ++index)
    {
        auto id = "amp" + std::to_string(index);
        document.graph.nodes.push_back({id, "talas.vca", {}});
        document.graph.connections.push_back({previous, "audioOut", id, "audioIn"});
        previous = std::move(id);
    }
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({previous, "audioOut", "out", "audioIn"});
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    bool compiled = false;
    runOnSmallStack(
        [&] {
            compiled = talas::runtime::compileProject(document, registry, {48000.0, 64}).ok();
        },
        512U * 1024U);
    require(compiled, "a long processing chain should compile on a 512 KiB worker stack");
}

void testEventValueDelayWithFeedbackRendersAcrossChanges()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(
        {"osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1"}}});
    document.graph.nodes.push_back(
        {"echo", "talas.delayWithFeedback", {{"delay", "!DelayTime"}, {"feedback", "0.5"}, {"mix", "0.5"}}});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections = {
        {"osc", "audioOut", "echo", "audioIn"},
        {"echo", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings{8.0, 4};
    settings.eventValues = {{"DelayTime", 0.25, talas::expr::ValueDomain::Seconds}};
    settings.eventValueChanges = {{8, "DelayTime", 0.5, talas::expr::ValueDomain::Seconds}};
    const auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "Event Value delay fixture should compile");
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 32);
    require(rendered.ok(),
        "an Event Value delay change must not fail the offline render: "
            + (rendered.diagnostics.empty() ? std::string{} : rendered.diagnostics.front().message));
}

void testMissingComponentFailsBeforeResourceMaterialization()
{
    talas::runtime::RenderSettings settings;
    settings.resourceSnapshots.push_back(nullptr);
    const auto result = talas::runtime::compileProject(
        makeMissingComponentAudioDocument(),
        makeRegistry(),
        std::move(settings));

    require(!result.ok(), "preserved missing component must not compile");
    require(
        !result.diagnostics.empty()
            && result.diagnostics.front().code
                == DiagnosticCode::GraphValidationFailed
            && result.diagnostics.front().nodeId == "missing1"
            && result.diagnostics.front().message.find(
                   "org.example.component")
                != std::string::npos,
        "missing-component availability must fail before invalid resource snapshots are materialized");
}

void testNestedMissingComponentReportsQualifiedScopeBeforeResources()
{
    talas::runtime::RenderSettings settings;
    settings.resourceSnapshots.push_back(nullptr);
    const auto result = talas::runtime::compileProject(
        makeNestedMissingComponentAudioDocument(),
        makeRegistry(),
        std::move(settings));

    require(
        !result.ok()
            && !result.diagnostics.empty()
            && result.diagnostics.front().code
                == DiagnosticCode::GraphValidationFailed
            && result.diagnostics.front().nodeId
                == "voice/missing1"
            && result.diagnostics.front().message.find(
                   "org.example.component")
                != std::string::npos,
        "nested missing component should fail before resources with a qualified Sound scope");
}

void testIncompatibleComponentReportsExactReasonBeforeResources()
{
    const auto document = makeMissingComponentAudioDocument();
    auto registry = makeRegistry();
    auto incompatible =
        talas::project::makeProjectPreservedComponentDescriptor(
            "org.example.component",
            *document.graph.nodes[1].preservedComponent);
    incompatible.typeVersion = "2.0.0";
    incompatible.stateSchemaVersion = 4;
    registry.addNodeType(std::move(incompatible));
    talas::runtime::RenderSettings settings;
    settings.resourceSnapshots.push_back(nullptr);

    const auto result = talas::runtime::compileProject(
        document,
        registry,
        std::move(settings));
    require(
        !result.ok()
            && !result.diagnostics.empty()
            && result.diagnostics.front().nodeId == "missing1"
            && result.diagnostics.front().message.find(
                   "type version mismatch")
                != std::string::npos,
        "installed incompatible component should report its exact mismatch before resources");
}

void classifyRuntimeFixtureNodes(talas::core::NodeRegistry& registry)
{
    for (const auto typeId : {"talas.oscillator", "talas.audioOutput"})
    {
        registry.setNodeTypeSupport(
            typeId,
            talas::core::NodeSupportLevel::Utility,
            "Runtime compile-plan test fixture.");
        registry.setNodeTypeOfflineSupport(
            typeId,
            talas::core::ExecutionSupport::Supported,
            talas::core::ExecutionBinding::LegacyAdapter);
    }
}

talas::core::NodeRegistry makeChoiceParameterRegistry()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {
            {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
            {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
        },
    });
    registry.addNodeType({
        "talas.audioOutput",
        "Audio Output",
        "IO",
        {{"audioIn", talas::core::PortType::Audio, true}},
        {},
        {
            talas::core::choiceParameter("mode", "power", {"power", "linear"}),
        },
    });
    classifyRuntimeFixtureNodes(registry);
    return registry;
}

talas::core::NodeRegistry makeBooleanParameterRegistry()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {
            {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
            {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
        },
    });
    registry.addNodeType({
        "talas.audioOutput",
        "Audio Output",
        "IO",
        {{"audioIn", talas::core::PortType::Audio, true}},
        {},
        {
            talas::core::booleanParameter("armed", true),
            talas::core::choiceParameter("mode", "power", {"power", "linear"}),
        },
    });
    classifyRuntimeFixtureNodes(registry);
    return registry;
}

talas::core::NodeRegistry makeResourceTextParameterRegistry()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {
            {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
            {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
        },
    });
    registry.addNodeType({
        "talas.audioOutput",
        "Audio Output",
        "IO",
        {{"audioIn", talas::core::PortType::Audio, true}},
        {},
        {
            talas::core::resourceParameter("sample", "Samples/Default.aif"),
            talas::core::textParameter("comment", ""),
        },
    });
    classifyRuntimeFixtureNodes(registry);
    return registry;
}

talas::project::ProjectDocument makeChoiceParameterAudioDocument(
    std::vector<talas::project::ProjectParameterExpression> outputParameters = {},
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput", std::move(outputParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeBooleanParameterAudioDocument(
    std::vector<talas::project::ProjectParameterExpression> outputParameters = {},
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput", std::move(outputParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeResourceTextParameterAudioDocument(
    std::vector<talas::project::ProjectParameterExpression> outputParameters = {},
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput", std::move(outputParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "out", "audioIn"));
    return document;
}

const talas::runtime::CompileStep& requireStep(const talas::runtime::RuntimePlan& plan, std::string_view nodeId)
{
    const auto found = std::ranges::find_if(plan.steps, [&](const talas::runtime::CompileStep& step) {
        return step.nodeId == nodeId;
    });

    require(found != plan.steps.end(), "plan should contain step for node " + std::string(nodeId));
    return *found;
}

std::size_t requireStepIndex(const talas::runtime::RuntimePlan& plan, std::string_view nodeId)
{
    for (std::size_t index = 0; index < plan.steps.size(); ++index)
    {
        if (plan.steps[index].nodeId == nodeId)
            return index;
    }

    throw std::runtime_error("plan should contain step for node " + std::string(nodeId));
}

const talas::runtime::ParameterValue& requireParameter(const talas::runtime::CompileStep& step, std::string_view parameterId)
{
    const auto found = std::ranges::find_if(step.parameters, [&](const talas::runtime::ParameterValue& parameter) {
        return parameter.id == parameterId;
    });

    require(found != step.parameters.end(), step.nodeId + " should contain parameter " + std::string(parameterId));
    return *found;
}

const talas::runtime::ParameterValue& requireChoiceParameter(const talas::runtime::CompileStep& step, std::string_view parameterId, std::string_view expectedChoice)
{
    const auto& parameter = requireParameter(step, parameterId);
    require(parameter.choice.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should contain a choice value");
    require(*parameter.choice == expectedChoice, step.nodeId + " parameter " + std::string(parameterId) + " should contain expected choice");
    return parameter;
}

const talas::runtime::ParameterValue& requireBooleanParameter(const talas::runtime::CompileStep& step, std::string_view parameterId, bool expectedValue)
{
    const auto& parameter = requireParameter(step, parameterId);
    require(parameter.boolean.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should contain a boolean value");
    require(*parameter.boolean == expectedValue, step.nodeId + " parameter " + std::string(parameterId) + " should contain expected boolean");
    require(!parameter.choice.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should not contain a choice value");
    return parameter;
}

const talas::runtime::ParameterValue& requireResourceParameter(const talas::runtime::CompileStep& step, std::string_view parameterId, std::string_view expectedResource)
{
    const auto& parameter = requireParameter(step, parameterId);
    require(parameter.resource.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should contain a resource value");
    require(*parameter.resource == expectedResource, step.nodeId + " parameter " + std::string(parameterId) + " should contain expected resource");
    require(!parameter.choice.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should not contain a choice value");
    require(!parameter.boolean.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should not contain a boolean value");
    return parameter;
}

const talas::runtime::ParameterValue& requireTextParameter(const talas::runtime::CompileStep& step, std::string_view parameterId, std::string_view expectedText)
{
    const auto& parameter = requireParameter(step, parameterId);
    require(parameter.text.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should contain a text value");
    require(*parameter.text == expectedText, step.nodeId + " parameter " + std::string(parameterId) + " should contain expected text");
    require(!parameter.choice.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should not contain a choice value");
    require(!parameter.boolean.has_value(), step.nodeId + " parameter " + std::string(parameterId) + " should not contain a boolean value");
    return parameter;
}

const talas::runtime::InputBinding& requireInputBinding(const talas::runtime::CompileStep& step, std::string_view inputPortId)
{
    const auto found = std::ranges::find_if(step.inputs, [&](const talas::runtime::InputBinding& input) {
        return input.inputPortId == inputPortId;
    });

    require(found != step.inputs.end(), step.nodeId + " should contain input binding for " + std::string(inputPortId));
    return *found;
}

bool hasBuffer(const talas::runtime::RuntimePlan& plan, std::string_view nodeId, std::string_view portId)
{
    return std::ranges::any_of(plan.buffers, [&](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == nodeId && buffer.portId == portId;
    });
}

bool hasDiagnostic(const talas::runtime::CompileResult& result, DiagnosticCode code, std::string_view nodeId = {}, std::string_view parameterId = {})
{
    return std::ranges::any_of(result.diagnostics, [&](const talas::runtime::Diagnostic& diagnostic) {
        if (diagnostic.code != code)
            return false;
        if (!nodeId.empty() && diagnostic.nodeId != nodeId)
            return false;
        if (!parameterId.empty() && diagnostic.parameterId != parameterId)
            return false;
        return true;
    });
}

std::string compileDiagnosticsSummary(const talas::runtime::CompileResult& result)
{
    std::string message = " diagnostics:";
    for (const auto& diagnostic : result.diagnostics)
    {
        message += " code=" + std::to_string(static_cast<int>(diagnostic.code));
        message += " node=" + diagnostic.nodeId;
        message += " parameter=" + diagnostic.parameterId;
        message += " message=" + diagnostic.message;
    }
    return message;
}

void requireCompileFailure(const talas::runtime::CompileResult& result,
                           DiagnosticCode code,
                           std::string_view nodeId = {},
                           std::string_view parameterId = {},
                           std::source_location location = std::source_location::current())
{
    require(!result.ok(), std::string{"compile should fail at "} + location.file_name()
        + ":" + std::to_string(location.line()) + " node=" + std::string{nodeId}
        + " parameter=" + std::string{parameterId});
    require(!result.graph.has_value(), "failed compile should not return a prepared graph");
    if (!hasDiagnostic(result, code, nodeId, parameterId))
    {
        throw std::runtime_error("compile should report expected diagnostic;" + compileDiagnosticsSummary(result));
    }
}

void testScrambledDocumentCompilesIntoTopologicalPlanAndReachableBuffers()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("unusedSample", "talas.sample"));
    document.graph.nodes.push_back(node("add", "talas.add"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("right", "talas.constant", {{"value", "2.0"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.nodes.push_back(node("left", "talas.constant", {{"value", "3.0"}}));
    document.graph.connections.push_back(connection("add", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("left", "value", "add", "left"));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("right", "value", "add", "right"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "scrambled reachable graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.steps.size() == 6, "plan should contain reachable nodes and audio output only");
    require(requireStepIndex(plan, "left") < requireStepIndex(plan, "add"), "left constant should run before add");
    require(requireStepIndex(plan, "right") < requireStepIndex(plan, "add"), "right constant should run before add");
    require(requireStepIndex(plan, "add") < requireStepIndex(plan, "vca"), "add should run before vca");
    require(requireStepIndex(plan, "osc") < requireStepIndex(plan, "vca"), "oscillator should run before vca");
    require(requireStepIndex(plan, "vca") < requireStepIndex(plan, "out"), "vca should run before audio output");

    require(plan.buffers.size() == 5, "plan should allocate one buffer per reachable produced output");
    require(hasBuffer(plan, "left", "value"), "left constant output should have a buffer");
    require(hasBuffer(plan, "right", "value"), "right constant output should have a buffer");
    require(hasBuffer(plan, "add", "value"), "add output should have a buffer");
    require(hasBuffer(plan, "osc", "audioOut"), "oscillator output should have a buffer");
    require(hasBuffer(plan, "vca", "audioOut"), "vca output should have a buffer");
    require(!hasBuffer(plan, "out", "audioIn"), "audio output input should not allocate an output buffer");
    require(!hasBuffer(plan, "unusedSample", "audioOut"), "unreachable unsupported node should not allocate a buffer");
    require(plan.outputBufferIndex < plan.buffers.size(), "output buffer index should refer to a real buffer");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "vca", "output buffer should be driven by vca");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "output buffer should be vca audioOut");

    const auto& add = requireStep(plan, "add");
    const auto& addLeft = requireInputBinding(add, "left");
    require(addLeft.sourceNodeId == "left", "add.left should bind to left constant");
    require(addLeft.sourcePortId == "value", "add.left should bind to left value output");
    require(addLeft.bufferIndex < plan.buffers.size(), "add.left buffer index should be valid");
    require(plan.buffers[addLeft.bufferIndex].nodeId == "left", "add.left buffer should point to left output");
    require(plan.buffers[addLeft.bufferIndex].portId == "value", "add.left buffer should point to left value");

    const auto& addRight = requireInputBinding(add, "right");
    require(addRight.sourceNodeId == "right", "add.right should bind to right constant");
    require(addRight.sourcePortId == "value", "add.right should bind to right value output");
    require(addRight.bufferIndex < plan.buffers.size(), "add.right buffer index should be valid");
    require(plan.buffers[addRight.bufferIndex].nodeId == "right", "add.right buffer should point to right output");
    require(plan.buffers[addRight.bufferIndex].portId == "value", "add.right buffer should point to right value");

    const auto& vca = requireStep(plan, "vca");
    const auto& vcaAudioIn = requireInputBinding(vca, "audioIn");
    require(vcaAudioIn.sourceNodeId == "osc", "vca.audioIn should bind to oscillator");
    require(vcaAudioIn.sourcePortId == "audioOut", "vca.audioIn should bind to oscillator audio output");
    require(vcaAudioIn.bufferIndex < plan.buffers.size(), "vca.audioIn buffer index should be valid");
    require(plan.buffers[vcaAudioIn.bufferIndex].nodeId == "osc", "vca.audioIn buffer should point to oscillator output");
    require(plan.buffers[vcaAudioIn.bufferIndex].portId == "audioOut", "vca.audioIn buffer should point to oscillator audioOut");

    const auto& vcaGain = requireInputBinding(vca, "gain");
    require(vcaGain.sourceNodeId == "add", "vca.gain should bind to add");
    require(vcaGain.sourcePortId == "value", "vca.gain should bind to add value output");
    require(vcaGain.bufferIndex < plan.buffers.size(), "vca.gain buffer index should be valid");
    require(plan.buffers[vcaGain.bufferIndex].nodeId == "add", "vca.gain buffer should point to add output");
    require(plan.buffers[vcaGain.bufferIndex].portId == "value", "vca.gain buffer should point to add value");
}

void testParkedUnsupportedNodeDoesNotFailCompile()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node("parkedSample", "talas.sample"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "parked unsupported node should not fail compile");
    const auto& plan = result.graph->plan();
    require(requireStepIndex(plan, "osc") < requireStepIndex(plan, "vca"), "reachable oscillator should still feed vca");
    require(!hasBuffer(plan, "parkedSample", "audioOut"), "parked unsupported node should not allocate buffers");
}

void testParkedInvalidNodeAndConnectionDoNotFailCompile()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node("parkedBad", "talas.missing"));
    document.graph.nodes.push_back(node("parkedVca", "talas.vca"));
    document.graph.nodes.push_back(node("parkedOsc1", "talas.oscillator"));
    document.graph.nodes.push_back(node("parkedOsc2", "talas.oscillator"));
    document.graph.connections.push_back(connection("deletedSource", "audioOut", "parkedVca", "audioIn"));
    document.graph.connections.push_back(connection("parkedOsc1", "audioOut", "parkedVca", "audioIn"));
    document.graph.connections.push_back(connection("parkedOsc2", "audioOut", "parkedVca", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "parked invalid node, stale connection, and duplicate input should not fail compile");
    const auto& plan = result.graph->plan();
    require(plan.steps.size() == 3, "plan should contain only audio output and its upstream nodes");
    require(!hasBuffer(plan, "parkedVca", "audioOut"), "parked invalid branch should not allocate buffers");
}

void testOmittedProjectParametersUseBuiltinDefaults()
{
    const auto result = talas::runtime::compileProject(makeBasicAudioDocument(), makeRegistry(), {});

    require(result.ok(), "document with omitted parameters should compile");
    const auto& plan = result.graph->plan();
    const auto& oscillator = requireStep(plan, "osc");
    const auto& vca = requireStep(plan, "vca");

    const auto& frequency = requireParameter(oscillator, "frequency");
    require(frequency.domain == talas::expr::ValueDomain::Hertz, "frequency should keep hertz domain");
    requireNear(frequency.value, 440.0, 1.0e-9, "omitted oscillator frequency should use builtin default");

    const auto& amplitude = requireParameter(oscillator, "amplitude");
    require(amplitude.domain == talas::expr::ValueDomain::Scalar, "amplitude should keep scalar domain");
    requireNear(amplitude.value, 1.0, 1.0e-12, "omitted oscillator amplitude should use builtin default");

    const auto& gain = requireParameter(vca, "gain");
    requireNear(gain.value, 1.0, 1.0e-12, "omitted vca gain should use builtin default");
}

void testProjectParameterOverridesWinOverDefaults()
{
    const auto result = talas::runtime::compileProject(
        makeBasicAudioDocument({{"frequency", "220 hz"}, {"amplitude", "0.25"}}, {{"gain", "0.5"}}),
        makeRegistry(),
        {});

    require(result.ok(), "document with parameter overrides should compile");
    const auto& plan = result.graph->plan();

    requireNear(requireParameter(requireStep(plan, "osc"), "frequency").value, 220.0, 1.0e-9, "frequency override should win");
    requireNear(requireParameter(requireStep(plan, "osc"), "amplitude").value, 0.25, 1.0e-12, "amplitude override should win");
    requireNear(requireParameter(requireStep(plan, "vca"), "gain").value, 0.5, 1.0e-12, "vca gain override should win");
}

void testInitialEventValueBindingsResolveTypedExpressions()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"Fader", 0.5, talas::expr::ValueDomain::Scalar},
    };

    const auto result = talas::runtime::compileProject(
        makeBasicAudioDocument({
            {"frequency", "!Fader * 12 nn + !Pitch"},
            {"amplitude", "!Fader"},
        }),
        makeRegistry(),
        settings);

    require(result.ok(), "typed initial Event Value expressions should compile");
    requireNear(
        requireParameter(requireStep(result.graph->plan(), "osc"), "frequency").value,
        440.0 * std::pow(2.0, -3.0 / 12.0),
        1.0e-9,
        "initial Event Values should resolve note-number expressions before hertz conversion");
    requireNear(requireParameter(requireStep(result.graph->plan(), "osc"), "amplitude").value, 0.5, 1.0e-12, "initial Event Value scalar should resolve");
}

void testInitialEventValueBindingsRejectUnknownAndInvalidSettings()
{
    auto result = talas::runtime::compileProject(
        makeBasicAudioDocument({{"frequency", "!Pitch"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "osc", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {{"!Pitch", 60.0, talas::expr::ValueDomain::NoteNumber}};
    result = talas::runtime::compileProject(makeBasicAudioDocument(), makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"Pitch", 61.0, talas::expr::ValueDomain::NoteNumber},
    };
    result = talas::runtime::compileProject(makeBasicAudioDocument(), makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValues = {{"Pitch", std::numeric_limits<double>::infinity(), talas::expr::ValueDomain::NoteNumber}};
    result = talas::runtime::compileProject(makeBasicAudioDocument(), makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);
}

void testRawMidiEventValueSchedulesCompileAndValidate()
{
    const auto makeRawMidiDocument = [](std::string bytes) {
        talas::project::ProjectDocument document;
        document.graph.nodes.push_back(node(
            "terminal",
            "talas.midiOutputEventInBytes",
            {textParameter("bytes", std::move(bytes))}));
        return document;
    };

    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {
        {"Velocity", 64.0, talas::expr::ValueDomain::Scalar},
        {"Unused", 0.0, talas::expr::ValueDomain::Scalar},
    };
    settings.eventValueChanges = {
        {2, "Velocity", 127.0, talas::expr::ValueDomain::Scalar},
        {3, "Unused", 1.0, talas::expr::ValueDomain::Scalar},
    };

    const auto document = makeRawMidiDocument("16r90 {!Velocity} 16r7f");
    auto result = talas::runtime::compileProject(document, makeRegistry(), settings);
    require(result.ok(), "scheduled raw MIDI Event Value expression should compile");

    settings.eventValueChanges = {{2, "Missing", 127.0, talas::expr::ValueDomain::Scalar}};
    result = talas::runtime::compileProject(document, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValueChanges = {{2, "Velocity", 127.0, talas::expr::ValueDomain::Hertz}};
    result = talas::runtime::compileProject(document, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValueChanges = {
        {3, "Velocity", 100.0, talas::expr::ValueDomain::Scalar},
        {2, "Velocity", 127.0, talas::expr::ValueDomain::Scalar},
    };
    result = talas::runtime::compileProject(document, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValueChanges = {
        {2, "Velocity", 100.0, talas::expr::ValueDomain::Scalar},
        {2, "Velocity", 127.0, talas::expr::ValueDomain::Scalar},
    };
    result = talas::runtime::compileProject(document, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValueChanges = {{2, "Velocity", std::numeric_limits<double>::infinity(), talas::expr::ValueDomain::Scalar}};
    result = talas::runtime::compileProject(document, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings.eventValueChanges.clear();
    const auto unresolved = makeRawMidiDocument("16r90 {!Missing} 16r7f");
    result = talas::runtime::compileProject(unresolved, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "bytes");

    std::string oversizedBytes;
    for (std::size_t index = 0; index < 257; ++index)
    {
        if (!oversizedBytes.empty())
            oversizedBytes += ' ';
        oversizedBytes += "0";
    }
    result = talas::runtime::compileProject(
        makeRawMidiDocument(std::move(oversizedBytes)),
        makeRegistry(),
        {});
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "terminal",
        "bytes");
}

void testConstantCompilePlanRetainsEventValueExpression()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node("gain", "talas.constant", {{"value", "!Gain"}}));
    document.graph.connections.push_back(connection("gain", "value", "vca", "gain"));

    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {{"Gain", 0.0, talas::expr::ValueDomain::Scalar}};
    const auto result = talas::runtime::compileProject(document, makeRegistry(), settings);

    require(result.ok(), "Event Value constant graph should compile");
    const auto& value = requireParameter(requireStep(result.graph->plan(), "gain"), "value");
    requireNear(value.value, 0.0, 1.0e-12, "Event Value constant should retain the initial resolved value");
    require(value.expression.has_value(), "Event Value constant should retain its parsed source expression");
    require(value.expression->eventReferences() == std::vector<std::string>({"Gain"}), "Event Value constant should retain referenced Event Values");
}

// A parameter that is a constant compiles to its
// value, a static field the Sound reads without computing anything as it
// plays (the renderers evaluate only a field reading a control, a variable
// or time, or keeping state); one that varies in time stays a program the
// Sound runs as it plays, so its points can change and be heard while it
// plays.
void testConstantParametersNeedLessComputationThanTimeVaryingOnes()
{
    const auto constant = talas::runtime::compileProject(
        makeBasicAudioDocument({{"amplitude", "0.25"}}, {{"gain", "0.5 * 0.5"}}), makeRegistry(), {});
    require(constant.ok(), "the Sound with constant parameters should compile");
    const auto& level = requireParameter(requireStep(constant.graph->plan(), "osc"), "amplitude");
    const auto& gain = requireParameter(requireStep(constant.graph->plan(), "vca"), "gain");
    requireNear(level.value, 0.25, 1.0e-12, "a constant amplitude compiles to its value");
    requireNear(gain.value, 0.25, 1.0e-12, "a constant gain written as arithmetic compiles to its value");
    const auto isStatic = [](const talas::runtime::ParameterValue& parameter) {
        return !parameter.expression.has_value()
            || (parameter.expression->eventReferences().empty() && parameter.expression->variableReferences().empty()
                && !parameter.expression->usesTime() && !parameter.expression->isStateful());
    };
    require(isStatic(level) && isStatic(gain),
        "a constant compiles to a static value, leaving nothing to compute as the Sound plays");

    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {{"Gain", 0.5, talas::expr::ValueDomain::Scalar}};
    auto timed = makeBasicAudioDocument({{"amplitude", "0.25 * (1 repeatingRamp: 2 s)"}}, {{"gain", "!Gain"}});
    timed.expressionLanguage = talas::expr::ExpressionLanguageVersion::ValV1;
    const auto varying = talas::runtime::compileProject(timed, makeRegistry(), settings);
    require(varying.ok(), "the Sound with time-varying parameters should compile: "
            + (varying.diagnostics.empty() ? std::string{} : varying.diagnostics.front().message));
    const auto& moving = requireParameter(requireStep(varying.graph->plan(), "osc"), "amplitude");
    const auto& controlled = requireParameter(requireStep(varying.graph->plan(), "vca"), "gain");
    require(!isStatic(moving) && moving.expression->usesTime() && moving.expression->instructionCount()
                > (level.expression.has_value() ? level.expression->instructionCount() : 0U),
        "a function of time compiles to a longer program, run as the Sound plays");
    require(!isStatic(controlled) && controlled.expression->eventReferences() == std::vector<std::string>({"Gain"}),
        "a parameter a control moves keeps the program that reads it");
}

void testChoiceParameterDefaultCompilesIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(makeChoiceParameterAudioDocument(), makeChoiceParameterRegistry(), {});

    require(result.ok(), "omitted choice parameter should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireChoiceParameter(output, "mode", "power");
}

void testChoiceParameterOverrideCompilesIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(
        makeChoiceParameterAudioDocument({choiceParameter("mode", "linear")}),
        makeChoiceParameterRegistry(),
        {});

    require(result.ok(), "choice parameter override should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireChoiceParameter(output, "mode", "linear");
}

void testInvalidChoiceParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeChoiceParameterAudioDocument({choiceParameter("mode", "wrong")}),
        makeChoiceParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "mode");
}

void testExpressionOverrideForChoiceParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeChoiceParameterAudioDocument({{"mode", "linear"}}),
        makeChoiceParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "mode");
}

void testExpressionAndChoiceOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeChoiceParameterAudioDocument({expressionAndChoiceParameter("mode", "0", "linear")}),
        makeChoiceParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "mode");
}

void testChoiceOverrideForExpressionParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeChoiceParameterAudioDocument({}, {choiceParameter("amplitude", "linear")}),
        makeChoiceParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "osc", "amplitude");
}

void testBooleanParameterDefaultCompilesIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(makeBooleanParameterAudioDocument(), makeBooleanParameterRegistry(), {});

    require(result.ok(), "omitted boolean parameter should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireBooleanParameter(output, "armed", true);
}

void testBooleanParameterOverrideCompilesIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({booleanParameter("armed", false)}),
        makeBooleanParameterRegistry(),
        {});

    require(result.ok(), "boolean parameter override should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireBooleanParameter(output, "armed", false);
}

void testExpressionOverrideForBooleanParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({{"armed", "1"}}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "armed");
}

void testExpressionAndBooleanOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({expressionAndBooleanParameter("armed", "1", true)}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "armed");
}

void testEmptyExpressionAndBooleanOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({expressionAndBooleanParameter("armed", "", true)}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "armed");
}

void testChoiceAndBooleanOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({choiceAndBooleanParameter("armed", "on", true)}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "armed");
}

void testBooleanOverrideForExpressionParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({}, {booleanParameter("amplitude", true)}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "osc", "amplitude");
}

void testBooleanOverrideForChoiceParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeBooleanParameterAudioDocument({booleanParameter("mode", true)}),
        makeBooleanParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "mode");
}

void testResourceTextParameterDefaultsCompileIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(makeResourceTextParameterAudioDocument(), makeResourceTextParameterRegistry(), {});

    require(result.ok(), "omitted resource/text parameters should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireResourceParameter(output, "sample", "Samples/Default.aif");
    requireTextParameter(output, "comment", "");
}

void testResourceTextParameterOverridesCompileIntoRuntimePlan()
{
    const auto result = talas::runtime::compileProject(
        makeResourceTextParameterAudioDocument({
            resourceParameter("sample", "Samples/Snare.aif"),
            textParameter("comment", "layer B"),
        }),
        makeResourceTextParameterRegistry(),
        {});

    require(result.ok(), "resource/text parameter overrides should compile");
    const auto& output = requireStep(result.graph->plan(), "out");
    requireResourceParameter(output, "sample", "Samples/Snare.aif");
    requireTextParameter(output, "comment", "layer B");
}

void testExpressionOverrideForResourceParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeResourceTextParameterAudioDocument({{"sample", "0"}}),
        makeResourceTextParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "sample");
}

void testExpressionAndResourceOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeResourceTextParameterAudioDocument({expressionAndResourceParameter("sample", "", "Samples/Snare.aif")}),
        makeResourceTextParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "sample");
}

void testBooleanAndTextOverrideFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeResourceTextParameterAudioDocument({booleanAndTextParameter("comment", true, "layer B")}),
        makeResourceTextParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "out", "comment");
}

void testResourceOverrideForExpressionParameterFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeResourceTextParameterAudioDocument({}, {resourceParameter("amplitude", "Samples/Snare.aif")}),
        makeResourceTextParameterRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "osc", "amplitude");
}

void testUnknownParameterOverrideFailsWithNodeAndParameter()
{
    auto document = makeBasicAudioDocument({{"detune", "12"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::UnknownParameter, "osc", "detune");
}

void testDuplicateParameterOverrideFailsWithNodeAndParameter()
{
    auto document = makeBasicAudioDocument({{"frequency", "220 hz"}, {"frequency", "330 hz"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::DuplicateParameter, "osc", "frequency");
}

void testInvalidParameterExpressionFailsWithoutDefaultFallback()
{
    auto document = makeBasicAudioDocument({{"frequency", "1 +"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::ParameterParseError, "osc", "frequency");
}

void testParameterEvaluationErrorFailsWithoutDefaultFallback()
{
    auto document = makeBasicAudioDocument({{"amplitude", "1 / 0"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "osc", "amplitude");
}

// A negative frequency reads the Oscillator's
// wavetable backwards, so it compiles.
void testNegativeOscillatorFrequencyCompiles()
{
    auto document = makeBasicAudioDocument({{"frequency", "-1 hz"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "a negative Oscillator frequency should compile");
}

void testInvalidReachableRampDurationFailsCompile()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.nodes.push_back(node("ramp", "talas.linearRamp", {{"duration", "0 s"}, {"start", "1"}, {"end", "5"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("ramp", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ramp", "duration");
}

void testNegativeReachableDelayFailsCompile()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("delay", "talas.delayWithFeedback", {{"delay", "-1 ms"}, {"feedback", "0"}, {"mix", "1"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "delay", "audioIn"));
    document.graph.connections.push_back(connection("delay", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "delay", "delay");
}

void testConnectedDelayOwnsNegativeStaticFallback()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("delay", "talas.delayWithFeedback", {
        {"delay", "-1 ms"}, {"feedback", "0"}, {"mix", "1"},
    }));
    document.graph.nodes.push_back(node("delayValue", "talas.constant", {
        {"value", "0.25"},
    }));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "delay", "audioIn"));
    document.graph.connections.push_back(connection(
        "delayValue", "value", "delay", "delay"));
    document.graph.connections.push_back(connection(
        "delay", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});

    require(result.ok(),
        "C140 connected delay should own its inert negative static fallback");
}

void testInvalidAveragingLowPassCutoffFailsCompile()
{
    auto makeDocument = [](std::string cutoff) {
        talas::project::ProjectDocument document;
        document.graph.nodes.push_back(node("out", "talas.audioOutput"));
        document.graph.nodes.push_back(node("avg", "talas.averagingLowPassFilter", {
            {"cutoff", std::move(cutoff)},
            resourceParameter("wavetable", "Private"),
            {"scale", "1.0"},
        }));
        document.graph.nodes.push_back(node("osc", "talas.oscillator"));
        document.graph.connections.push_back(connection("osc", "audioOut", "avg", "audioIn"));
        document.graph.connections.push_back(connection("avg", "audioOut", "out", "audioIn"));
        return document;
    };

    auto result = talas::runtime::compileProject(makeDocument("0 hz"), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "avg", "cutoff");

    result = talas::runtime::compileProject(makeDocument("-1 hz"), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "avg", "cutoff");
}

talas::project::ProjectDocument makeForcedProcessorAssignmentOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> forcedParameters = {{"processor", "1"}})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("forced", "talas.forcedProcessorAssignment", std::move(forcedParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "forced", "audioIn"));
    document.graph.connections.push_back(connection("forced", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeNestedForcedProcessorAssignmentOutputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "outer", "talas.forcedProcessorAssignment", {{"processor", "7"}}));
    document.graph.nodes.push_back(node(
        "inner", "talas.forcedProcessorAssignment", {{"processor", "3"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(
        connection("osc", "audioOut", "inner", "audioIn"));
    document.graph.connections.push_back(
        connection("inner", "audioOut", "outer", "audioIn"));
    document.graph.connections.push_back(
        connection("outer", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeNestedSoundProcessorPlacementDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "placementVoice";
    voice.displayName = "Placement Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "forced",
        "audioOut",
    });
    voice.graph.nodes.push_back(node("osc", "talas.oscillator"));
    voice.graph.nodes.push_back(node(
        "forced",
        "talas.forcedProcessorAssignment",
        {{"processor", "13"}}));
    voice.graph.connections.push_back(
        connection("osc", "audioOut", "forced", "audioIn"));
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "placementVoice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.connections.push_back(
        connection("voice1", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression>
midiVoiceParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides);

std::vector<talas::project::ProjectParameterExpression>
analogSequencerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides);

talas::project::ProjectDocument makePreparedMidiVoiceOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> midiParameters);

talas::runtime::RenderSettings midiVoiceCompileSettings();

talas::project::ProjectDocument makeConflictingForcedProcessorAssignmentDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mix", "talas.mixer"));
    document.graph.nodes.push_back(node(
        "left", "talas.forcedProcessorAssignment", {{"processor", "1"}}));
    document.graph.nodes.push_back(node(
        "right", "talas.forcedProcessorAssignment", {{"processor", "2"}}));
    document.graph.nodes.push_back(node("shared", "talas.oscillator"));
    document.graph.connections.push_back(
        connection("shared", "audioOut", "left", "audioIn"));
    document.graph.connections.push_back(
        connection("shared", "audioOut", "right", "audioIn"));
    document.graph.connections.push_back(
        connection("left", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(
        connection("right", "audioOut", "mix", "in2"));
    document.graph.connections.push_back(
        connection("mix", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeMultichannelOutputDocument(std::string outputTypeId, std::size_t channelCount)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", std::move(outputTypeId)));
    for (std::size_t channel = 1; channel <= channelCount; ++channel)
    {
        const auto id = "osc" + std::to_string(channel);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {{"frequency", std::to_string(channel) + " hz"}, {"amplitude", "1.0"}}));
        document.graph.connections.push_back(connection(id, "audioOut", "out", "out" + std::to_string(channel)));
    }
    return document;
}

std::vector<std::string> stereoInOutputPairPorts(std::string_view outputTypeId)
{
    if (outputTypeId == "talas.stereoInOutput4")
        return {"out12", "out34"};
    if (outputTypeId == "talas.stereoInOutput8")
        return {"out12", "out34", "out56", "out78"};
    return {};
}

talas::project::ProjectDocument makeStereoInOutputDocument(std::string outputTypeId)
{
    auto pairPorts = stereoInOutputPairPorts(outputTypeId);

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", std::move(outputTypeId)));
    for (std::size_t pairIndex = 0; pairIndex < pairPorts.size(); ++pairIndex)
    {
        const auto& pairPort = pairPorts[pairIndex];
        const auto id = "osc" + pairPort.substr(3);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", std::to_string(pairIndex + 1)}}));
        document.graph.connections.push_back(connection(id, "audioOut", "out", pairPort));
    }
    return document;
}

std::vector<talas::project::ProjectParameterExpression> matrixParameters(
    std::size_t channelCount,
    std::vector<std::string> rows = {})
{
    if (rows.empty())
    {
        rows.reserve(channelCount);
        for (std::size_t output = 1; output <= channelCount; ++output)
        {
            std::string row;
            for (std::size_t input = 1; input <= channelCount; ++input)
            {
                if (!row.empty())
                    row += ' ';
                row += input == output ? "1" : "0";
            }
            rows.push_back(std::move(row));
        }
    }

    std::vector<talas::project::ProjectParameterExpression> parameters;
    parameters.reserve(channelCount);
    for (std::size_t output = 1; output <= channelCount; ++output)
        parameters.push_back(textParameter("insToOut" + std::to_string(output), rows[output - 1]));
    return parameters;
}

talas::project::ProjectDocument makeMatrixTerminalDocument(std::string matrixTypeId, std::size_t channelCount)
{
    talas::project::ProjectDocument document;
    const auto matrixIsExplicitOutputSource = matrixTypeId == "talas.matrix4";
    document.graph.nodes.push_back(node("matrix", std::move(matrixTypeId), matrixParameters(channelCount)));
    if (matrixIsExplicitOutputSource)
        document.graph.nodes.push_back(node("out", "talas.output4"));
    for (std::size_t channel = 1; channel <= channelCount; ++channel)
    {
        const auto id = "osc" + std::to_string(channel);
        document.graph.nodes.push_back(node(id, "talas.oscillator", {{"frequency", std::to_string(channel) + " hz"}, {"amplitude", "1.0"}}));
        document.graph.connections.push_back(connection(id, "audioOut", "matrix", "in" + std::to_string(channel)));
        if (matrixIsExplicitOutputSource)
            document.graph.connections.push_back(connection("matrix", "out" + std::to_string(channel), "out", "out" + std::to_string(channel)));
    }
    return document;
}

void testStereoAudioOutputCapturesRightBufferIndex()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("left", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("right", "talas.oscillator", {{"frequency", "0.5 hz"}, {"amplitude", "2.0"}}));
    document.graph.connections.push_back(connection("left", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("right", "audioOut", "out", "rightIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "stereo audio output graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "left", "left output buffer should point to left oscillator");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "left output buffer should point to audioOut");
    require(plan.rightOutputBufferIndex.has_value(), "right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "right", "right output buffer should point to right oscillator");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "audioOut", "right output buffer should point to audioOut");
}

void requireMultichannelOutputPlan(const talas::runtime::RuntimePlan& plan, std::size_t channelCount)
{
    require(plan.outputBufferIndices.size() == channelCount, "multichannel output should expose every channel buffer index");
    require(plan.outputBufferIndex == plan.outputBufferIndices.front(), "first multichannel output should mirror legacy outputBufferIndex");
    if (channelCount > 1)
    {
        require(plan.rightOutputBufferIndex.has_value(), "second multichannel output should mirror legacy rightOutputBufferIndex");
        require(*plan.rightOutputBufferIndex == plan.outputBufferIndices[1], "right output index should mirror second channel");
    }

    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto bufferIndex = plan.outputBufferIndices[channel];
        require(bufferIndex < plan.buffers.size(), "multichannel output buffer index should be valid");
        require(plan.buffers[bufferIndex].nodeId == "osc" + std::to_string(channel + 1), "multichannel output buffer should point to matching oscillator");
        require(plan.buffers[bufferIndex].portId == "audioOut", "multichannel output buffer should point to oscillator audioOut");
    }
}

void testOutput4GraphCompilesWithFourOutputBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeMultichannelOutputDocument("talas.output4", 4), makeRegistry(), {});

    require(result.ok(), "output4 graph should compile");
    requireMultichannelOutputPlan(result.graph->plan(), 4);
}

void testOutput6GraphCompilesWithSixOutputBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeMultichannelOutputDocument("talas.output6", 6), makeRegistry(), {});

    require(result.ok(), "output6 graph should compile");
    requireMultichannelOutputPlan(result.graph->plan(), 6);
}

void testSixOutputSoundPreparesExactTerminal()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "sixChannels";
    sound.displayName = "Six Channels";
    for (std::size_t channel = 1; channel <= 6; ++channel)
    {
        const auto nodeId = "osc" + std::to_string(channel);
        sound.graph.nodes.push_back(node(nodeId, "talas.oscillator"));
        sound.outputs.push_back({
            "audio" + std::to_string(channel),
            talas::core::PortType::Audio,
            nodeId,
            "audioOut",
        });
    }
    document.soundDefinitions.push_back(std::move(sound));
    const auto prepared = talas::project::projectSoundPreparationDocument(
        document, "sixChannels");
    require(prepared.ok() && prepared.outputChannelCount == 6,
        "six-output Sound should prepare six exact Audio channels");
    const auto terminal = std::ranges::find(
        prepared.document.graph.nodes,
        std::string{"talas.output6"},
        &talas::project::ProjectNode::typeId);
    require(terminal != prepared.document.graph.nodes.end(),
        "six-output Sound should select Output 6");
    const auto compiled = talas::runtime::compileProject(
        prepared.document, makeRegistry(), {});
    require(compiled.ok()
            && compiled.graph->plan().outputBufferIndices.size() == 6,
        "prepared six-output Sound should compile all channels");
}

void testOutput8GraphCompilesWithEightOutputBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeMultichannelOutputDocument("talas.output8", 8), makeRegistry(), {});

    require(result.ok(), "output8 graph should compile");
    requireMultichannelOutputPlan(result.graph->plan(), 8);
}

void requireStereoInOutputPlan(
    const talas::runtime::RuntimePlan& plan,
    std::string_view outputTypeId,
    const std::vector<std::string>& pairPorts)
{
    const auto channelCount = pairPorts.size() * 2;
    const auto& terminal = requireStep(plan, "out");
    require(
        terminal.typeId == outputTypeId
            && terminal.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && terminal.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && !terminal.supportsReset
            && !terminal.kernelKind.has_value()
            && !terminal.preparedIsolatedComponent
            && terminal.parameters.empty()
            && terminal.inputs.size() == pairPorts.size()
            && terminal.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && terminal.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>(
                    pairPorts.size(), std::size_t{0})
            && terminal.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && terminal.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        std::string{outputTypeId}
            + " should preserve exact C28 prepared terminal authority");
    for (std::size_t pairIndex = 0; pairIndex < pairPorts.size(); ++pairIndex)
    {
        const auto& input = terminal.inputs[pairIndex];
        require(
            input.inputPortId == pairPorts[pairIndex]
                && input.sourceNodeId
                    == "osc" + pairPorts[pairIndex].substr(3)
                && input.sourcePortId == "audioOut"
                && input.sourceScopeId == talas::runtime::globalRuntimeScope
                && input.bufferIndex < plan.buffers.size()
                && plan.buffers[input.bufferIndex].type
                    == talas::core::PortType::Audio,
            std::string{outputTypeId}
                + " should preserve exact required paired-input order");
    }
    require(plan.outputBufferIndices.size() == channelCount, "stereo-in output should expose every paired channel index");
    require(plan.outputBufferIndex == plan.outputBufferIndices.front(), "first stereo-in output should mirror legacy outputBufferIndex");
    require(plan.rightOutputBufferIndex.has_value(), "second stereo-in output should mirror legacy rightOutputBufferIndex");
    require(*plan.rightOutputBufferIndex == plan.outputBufferIndices[1], "right output index should mirror second paired channel");

    for (std::size_t pairIndex = 0; pairIndex < pairPorts.size(); ++pairIndex)
    {
        const auto expectedNodeId = "osc" + pairPorts[pairIndex].substr(3);
        for (std::size_t side = 0; side < 2; ++side)
        {
            const auto channel = (pairIndex * 2) + side;
            const auto bufferIndex = plan.outputBufferIndices[channel];
            require(bufferIndex < plan.buffers.size(), "stereo-in output buffer index should be valid");
            require(plan.buffers[bufferIndex].nodeId == expectedNodeId, "stereo-in paired channels should point to matching oscillator");
            require(plan.buffers[bufferIndex].portId == "audioOut", "stereo-in paired channels should point to oscillator audioOut");
        }
        require(
            plan.outputBufferIndices[pairIndex * 2] == plan.outputBufferIndices[(pairIndex * 2) + 1],
            "stereo-in paired channels should duplicate the mono pair input buffer");
    }
}

void testStereoInOutput4GraphCompilesWithDuplicatedPairBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeStereoInOutputDocument("talas.stereoInOutput4"), makeRegistry(), {});

    require(result.ok(), "stereoInOutput4 graph should compile");
    requireStereoInOutputPlan(
        result.graph->plan(),
        "talas.stereoInOutput4",
        stereoInOutputPairPorts("talas.stereoInOutput4"));
}

void testStereoInOutput8GraphCompilesWithDuplicatedPairBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeStereoInOutputDocument("talas.stereoInOutput8"), makeRegistry(), {});

    require(result.ok(), "stereoInOutput8 graph should compile");
    requireStereoInOutputPlan(
        result.graph->plan(),
        "talas.stereoInOutput8",
        stereoInOutputPairPorts("talas.stereoInOutput8"));
}

void requireMatrixTerminalPlan(const talas::runtime::RuntimePlan& plan, std::size_t channelCount)
{
    require(plan.outputBufferIndices.size() == channelCount, "matrix terminal should expose every channel buffer index");
    require(plan.outputBufferIndex == plan.outputBufferIndices.front(), "first matrix terminal output should mirror legacy outputBufferIndex");
    if (channelCount > 1)
    {
        require(plan.rightOutputBufferIndex.has_value(), "second matrix terminal output should mirror legacy rightOutputBufferIndex");
        require(*plan.rightOutputBufferIndex == plan.outputBufferIndices[1], "right output index should mirror second matrix output channel");
    }

    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        const auto bufferIndex = plan.outputBufferIndices[channel];
        require(bufferIndex < plan.buffers.size(), "matrix terminal output buffer index should be valid");
        require(plan.buffers[bufferIndex].nodeId == "matrix", "matrix terminal output buffer should point to matrix node");
        require(plan.buffers[bufferIndex].portId == "out" + std::to_string(channel + 1), "matrix terminal output buffer should point to matching out port");
    }
}

void testMatrix4GraphCompilesWithFourOutputBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeMatrixTerminalDocument("talas.matrix4", 4), makeRegistry(), {});

    require(result.ok(), "matrix4 -> output4 graph should compile");
    requireMatrixTerminalPlan(result.graph->plan(), 4);
}

void testMatrix8GraphCompilesWithEightOutputBufferIndices()
{
    const auto result = talas::runtime::compileProject(makeMatrixTerminalDocument("talas.matrix8", 8), makeRegistry(), {});

    require(result.ok(), "matrix8 graph should compile");
    requireMatrixTerminalPlan(result.graph->plan(), 8);
}

void testForcedProcessorAssignmentGraphCompilesAsAudioPassthrough()
{
    const auto result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument(), makeRegistry(), {});

    require(result.ok(), "forced processor assignment graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "forced processor assignment output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "forced", "forced processor assignment output buffer should point to forced node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "forced processor assignment output buffer should point to audioOut");

    const auto& forced = requireStep(plan, "forced");
    const auto& audioIn = requireInputBinding(forced, "audioIn");
    require(audioIn.sourceNodeId == "osc", "forced processor assignment input should bind oscillator");
    require(audioIn.sourcePortId == "audioOut", "forced processor assignment input should bind oscillator audio output");

    require(
        plan.processorPlacement.stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{1, 1, 0},
        "forced processor assignment should publish exact closure placement");
    require(
        plan.processorPlacement.domains.size() == 2
            && plan.processorPlacement.transfers.size() == 1,
        "forced processor assignment should publish placed/host domains and one transfer");
    require(
        talas::runtime::validateProcessorPlacementPlan(plan).ok(),
        "compiled processor placement should validate independently");
}

void testNestedForcedProcessorAssignmentPublishesBoundariesAndTransfers()
{
    const auto result = talas::runtime::compileProject(
        makeNestedForcedProcessorAssignmentOutputDocument(),
        makeRegistry(),
        {});
    require(result.ok(), "nested forced processor assignment should compile");
    const auto& plan = result.graph->plan();
    require(
        plan.processorPlacement.stepProcessors
            == std::vector<talas::runtime::LogicalProcessorId>{3, 3, 7, 0},
        "nested forced processor assignment should stop at the inner boundary");
    require(
        plan.processorPlacement.domains.size() == 3
            && plan.processorPlacement.transfers.size() == 2
            && plan.processorPlacement.transfers[0].sourceProcessorId == 3
            && plan.processorPlacement.transfers[0].targetProcessorId == 7
            && plan.processorPlacement.transfers[1].sourceProcessorId == 7
            && plan.processorPlacement.transfers[1].targetProcessorId == 0,
        "nested forced processor assignment should publish canonical cross-domain transfers");
}

void testProcessorPlacementIsStableAcrossInsertionPermutations()
{
    auto forward = makeNestedForcedProcessorAssignmentOutputDocument();
    auto reversed = forward;
    std::ranges::reverse(reversed.graph.nodes);
    std::ranges::reverse(reversed.graph.connections);

    const auto first =
        talas::runtime::compileProject(forward, makeRegistry(), {});
    const auto second =
        talas::runtime::compileProject(reversed, makeRegistry(), {});
    require(
        first.ok() && second.ok(),
        "processor placement insertion permutations should compile");
    require(
        first.graph->plan().processorPlacement
            == second.graph->plan().processorPlacement,
        "processor placement should be canonical across insertion permutations");
}

void testConflictingForcedProcessorAssignmentClosureFailsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeConflictingForcedProcessorAssignmentDocument(),
        makeRegistry(),
        {});
    requireCompileFailure(
        result,
        DiagnosticCode::GraphValidationFailed,
        "shared");
}

void testForcedProcessorAssignmentProcessorCompilesToItsStartingValue()
{
    // Processor is a constant field: an Event Value
    // there compiles to its starting value, so placement stays immutable.
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {
        {"P", 7.0, talas::expr::ValueDomain::Scalar},
    };
    const auto result = talas::runtime::compileProject(
        makeForcedProcessorAssignmentOutputDocument({{"processor", "!P"}}),
        makeRegistry(),
        settings);
    require(result.ok(), "an Event Value processor should compile to its starting value");
    const auto& plan = result.graph->plan();
    const auto forced = std::ranges::find(plan.steps, std::string_view{"forced"},
        &talas::runtime::CompileStep::nodeId);
    require(forced != plan.steps.end(), "forced assignment should compile to a step");
    const auto processor = std::ranges::find(forced->parameters, std::string_view{"processor"},
        &talas::runtime::ParameterValue::id);
    require(processor != forced->parameters.end() && processor->value == 7.0
            && processor->expression.has_value()
            && processor->expression->eventReferences().empty(),
        "an Event Value processor should compile to the constant 7");
}

void testProcessorPlacementUsesExpandedNestedSoundIdentity()
{
    const auto result = talas::runtime::compileProject(
        makeNestedSoundProcessorPlacementDocument(),
        makeRegistry(),
        {});
    require(result.ok(), "nested Sound processor placement should compile");
    const auto& plan = result.graph->plan();
    const auto forced = std::ranges::find(
        plan.steps,
        std::string{"talas.forcedProcessorAssignment"},
        &talas::runtime::CompileStep::typeId);
    const auto oscillator = std::ranges::find(
        plan.steps,
        std::string{"talas.oscillator"},
        &talas::runtime::CompileStep::typeId);
    require(
        forced != plan.steps.end()
            && oscillator != plan.steps.end()
            && forced->scopeId != talas::runtime::globalRuntimeScope
            && forced->scopeId == oscillator->scopeId,
        "nested placement should retain one exact expanded Sound scope");
    const auto forcedIndex = static_cast<std::size_t>(
        forced - plan.steps.begin());
    const auto oscillatorIndex = static_cast<std::size_t>(
        oscillator - plan.steps.begin());
    require(
        plan.processorPlacement.stepProcessors[forcedIndex] == 13
            && plan.processorPlacement.stepProcessors[oscillatorIndex] == 13,
        "nested placement should own the expanded Sound boundary and closure");
}

void testProcessorPlacementUsesExpandedMidiVoiceIdentities()
{
    auto document = makePreparedMidiVoiceOutputDocument(
        midiVoiceParameters({{"polyphony", "2"}}));
    document.graph.nodes.push_back(node(
        "forced",
        "talas.forcedProcessorAssignment",
        {{"processor", "17"}}));
    auto regionOutput = std::ranges::find_if(
        document.graph.connections,
        [](const auto& graphConnection) {
            return graphConnection.fromNode == "vca"
                && graphConnection.toNode == "midi";
        });
    require(
        regionOutput != document.graph.connections.end(),
        "MIDI voice placement fixture should expose its region output");
    regionOutput->toNode = "forced";
    regionOutput->toPort = "audioIn";
    document.graph.connections.push_back(
        connection("forced", "audioOut", "midi", "audioIn"));

    const auto result = talas::runtime::compileProject(
        document,
        makeRegistry(),
        midiVoiceCompileSettings());
    require(result.ok(), "MIDI voice processor placement should compile");
    const auto& plan = result.graph->plan();
    std::vector<talas::runtime::RuntimeScopeId> forcedScopes;
    for (std::size_t index = 0; index < plan.steps.size(); ++index)
    {
        if (plan.steps[index].typeId != "talas.forcedProcessorAssignment")
            continue;
        forcedScopes.push_back(plan.steps[index].scopeId);
        require(
            plan.steps[index].scopeId
                    != talas::runtime::globalRuntimeScope
                && plan.processorPlacement.stepProcessors[index] == 17,
            "each prepared MIDI voice boundary should retain its exact logical processor");
    }
    require(
        forcedScopes.size() == 2
            && forcedScopes[0] != forcedScopes[1],
        "polyphony two should publish two distinct expanded placement identities");
}

void testProcessorPlacementUsesScoreSoundPreparationIdentity()
{
    const auto document = makeNestedSoundProcessorPlacementDocument();
    const auto scoreSource =
        talas::project::projectSoundPreparationDocument(
            document,
            "placementVoice",
            {},
            1,
            "scoreClipPlacementSource");
    require(
        scoreSource.ok(),
        "Score placement source should prepare its exact Sound document");
    const auto result = talas::runtime::compileProject(
        scoreSource.document,
        makeRegistry(),
        {});
    require(result.ok(), "Score Sound processor placement should compile");
    const auto& plan = result.graph->plan();
    const auto forced = std::ranges::find(
        plan.steps,
        std::string{"talas.forcedProcessorAssignment"},
        &talas::runtime::CompileStep::typeId);
    require(
        forced != plan.steps.end()
            && forced->scopeId != talas::runtime::globalRuntimeScope
            && plan.processorPlacement.stepProcessors[
                static_cast<std::size_t>(forced - plan.steps.begin())]
                == 13,
        "Score materialization source should retain the expanded Sound placement identity");
}

talas::project::ProjectDocument makePanStereoOutputDocument(std::vector<talas::project::ProjectParameterExpression> panParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("pan", "talas.pan", std::move(panParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "pan", "audioIn"));
    document.graph.connections.push_back(connection("pan", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("pan", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeFilterOutputDocument(std::vector<talas::project::ProjectParameterExpression> filterParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("filter", "talas.filter", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "100 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "filter", "audioIn"));
    document.graph.connections.push_back(connection("filter", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFirFilterOutputDocument(std::vector<talas::project::ProjectParameterExpression> filterParameters = {
    textParameter("coefficients", "0.5 0.25"),
})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("fir", "talas.firFilter", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "fir", "audioIn"));
    document.graph.connections.push_back(connection("fir", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeShelvingFilterOutputDocument(
    std::string nodeId,
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        {"cutoffFreq", "500 hz"},
        {"boostOrCut", "6"},
        {"scale", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(nodeId, std::move(typeId), std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1000 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", nodeId, "audioIn"));
    document.graph.connections.push_back(connection(nodeId, "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePresenceFilterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        {"centerFreq", "500 hz"},
        {"bandwidth", "200 hz"},
        {"boostOrCut", "6"},
        {"scale", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("presence", "talas.presenceFilter", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "500 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "presence", "audioIn"));
    document.graph.connections.push_back(connection("presence", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTwoFormantElementOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        {"formant1", "500 hz"},
        {"bandwidth1", "100 hz"},
        {"scale1", "1.0"},
        {"formant2", "1500 hz"},
        {"bandwidth2", "200 hz"},
        {"scale2", "0.5"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("formants", "talas.twoFormantElement", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "500 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "formants", "audioIn"));
    document.graph.connections.push_back(connection("formants", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTwoFormantVoiceElementOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> voiceParameters = {
        {"frequency", "200 hz"},
        {"formant1", "500 hz"},
        {"bandwidth1", "100 hz"},
        {"scale1", "1.0"},
        {"formant2", "1500 hz"},
        {"bandwidth2", "200 hz"},
        {"scale2", "0.5"},
        {"seed", "123"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("voice", "talas.twoFormantVoiceElement", std::move(voiceParameters)));
    document.graph.connections.push_back(connection("voice", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeDualParallelTwoPoleFilterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        textParameter("pole1", "0.5 % 0.25"),
        {"scale1", "1.0"},
        textParameter("pole2", "0.25 % -0.5"),
        {"scale2", "0.5"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("twoPole", "talas.dualParallelTwoPoleFilter", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "500 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "twoPole", "audioIn"));
    document.graph.connections.push_back(connection("twoPole", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeHarmonicResonatorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> resonatorParameters = {
        {"decayTime", "0.25 s"},
        {"brightness", "1.0"},
        {"frequency", "4 hz"},
        resourceParameter("wavetable", "Private"),
        booleanParameter("prezero", true),
        {"scale", "0.75"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("resonator", "talas.harmonicResonator", std::move(resonatorParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "resonator", "audioIn"));
    document.graph.connections.push_back(connection("resonator", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeReResonatorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> resonatorParameters = {
        resourceParameter("wavetable", "Tables/RE"),
        {"timeIndex", "0.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("resonator", "talas.reResonator", std::move(resonatorParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "resonator", "audioIn"));
    document.graph.connections.push_back(connection("resonator", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeGraphicEqOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> eqParameters = {
        {"cf250Hz", "1.0"},
        {"cf500Hz", "2.0"},
        {"cf1000Hz", "1.0"},
        {"cf2000Hz", "1.0"},
        {"cf4000Hz", "1.0"},
        {"cf8000Hz", "1.0"},
        {"cf16000Hz", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("eq", "talas.graphicEq", std::move(eqParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "500 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "eq", "audioIn"));
    document.graph.connections.push_back(connection("eq", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeVcfOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        {"amplitude", "0.75"},
        {"cutoff", "500 hz"},
        {"cutoffModulator", "0.0"},
        {"cutoffModRange", "100 hz"},
        {"resonance", "0.25"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vcf", "talas.vcf", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "500 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vcf", "audioIn"));
    document.graph.connections.push_back(connection("vcf", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeSingleSideBandRmOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> shifterParameters = {
        {"frequency", "440 hz"},
        {"freqScale", "2.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("ssb", "talas.singleSideBandRm", std::move(shifterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "ssb", "audioIn"));
    document.graph.connections.push_back(connection("ssb", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFrequencyScaleOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> scaleParameters = {
        {"frequencyScale", "1.0"},
        {"maxScale", "1.0"},
        resourceParameter("window", "Private"),
        {"delay", "0"},
    })
{
    // The phase vocoder, as projects before format 83 shift.
    if (std::ranges::none_of(scaleParameters, [](const auto& parameter) {
            return parameter.parameterId == "scaling";
        }))
        scaleParameters.push_back(choiceParameter("scaling", "legacyVocoder"));
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scale", "talas.frequencyScale", std::move(scaleParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("tracker", "talas.constant", {{"value", "440"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "scale", "audioIn"));
    document.graph.connections.push_back(connection("tracker", "value", "scale", "freqTracker"));
    document.graph.connections.push_back(connection("scale", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeMonotonizerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "440 hz"},
        {"minInputPitch", "440 hz"},
        {"maxInputPitch", "440 hz"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mono", "talas.monotonizer", std::move(parameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "mono", "audioIn"));
    document.graph.connections.push_back(connection("mono", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> timeFrequencyScaleParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequencyScale", "0.5"},
        {"rate", "0.5"},
        {"gate", "1.0"},
        {"minInputFreq", "20 hz"},
        {"maxInputFreq", "2000 hz"},
        {"maxFreqScale", "1.0"},
        {"detectors", "10"},
        booleanParameter("fromDisk", false),
        resourceParameter("sample", "Samples/TFS"),
        booleanParameter("fromMemoryWriter", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeTimeFrequencyScaleOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> tfsParameters = timeFrequencyScaleParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("tfs", "talas.timeFrequencyScale", std::move(tfsParameters)));
    document.graph.connections.push_back(connection("tfs", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeSimplePitchShifterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> shifterParameters = {
        {"interval", "0"},
        {"minInputPitch", "20 hz"},
        {"maxInputPitch", "2000 hz"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("pitch", "talas.simplePitchShifter", std::move(shifterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "pitch", "audioIn"));
    document.graph.connections.push_back(connection("pitch", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePhaseShiftBy90OutputDocument(
    std::vector<talas::project::ProjectParameterExpression> shifterParameters = {
        {"frequency", "440 hz"},
    })
{
    // These tests check the shifters' linear math: unclipped, as a project
    // before format 150 (later projects have them saturate).
    if (std::ranges::none_of(shifterParameters, [](const auto& parameter) { return parameter.parameterId == "overflow"; }))
        shifterParameters.push_back(choiceParameter("overflow", "unclipped"));
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("phase", "talas.phaseShiftBy90", std::move(shifterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "440 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "phase", "audioIn"));
    document.graph.connections.push_back(connection("phase", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeAnalysisFilterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> analysisParameters = {
        {"fundamental", "100 hz"},
        {"harmonic", "2"},
    },
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = {
        {"frequency", "200 hz"},
        {"amplitude", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("analysis", "talas.analysisFilter", std::move(analysisParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "analysis", "audioIn"));
    document.graph.connections.push_back(connection("analysis", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("analysis", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeAudioInputOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> inputParameters = {
        booleanParameter("channel1", true),
        booleanParameter("channel2", false),
        booleanParameter("channel3", false),
        booleanParameter("channel4", false),
        booleanParameter("channel5", false),
        booleanParameter("channel6", false),
        booleanParameter("channel7", false),
        booleanParameter("channel8", false),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("input", "talas.audioInput", std::move(inputParameters)));
    document.graph.connections.push_back(connection("input", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> genericSourceParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("source", "live input"),
        booleanParameter("leftChannel", true),
        booleanParameter("rightChannel", true),
        resourceParameter("sample", ""),
        booleanParameter("autoloop", false),
        {"trigger", "1.0"},
        {"attackTime", "0 s"},
        {"releaseTime", "0 s"},
        {"scale", "0.5"},
        {"frequency", "440 hz"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeGenericSourceOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sourceParameters = genericSourceParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("source", "talas.genericSource", std::move(sourceParameters)));
    document.graph.connections.push_back(connection("source", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("source", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> midiMapperParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("map", ""),
        choiceParameter("voiceModel", "legacyPassthrough"),
        {"left", "0.5"},
        {"right", "0.25"},
        {"channel", "1"},
        textParameter("source", ""),
        resourceParameter("midiFile", ""),
        {"polyphony", "1"},
        {"lowPitch", "0 nn"},
        {"highPitch", "127 nn"},
        textParameter("script", ""),
        booleanParameter("shared", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> midiVoiceParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"left", "0.5"},
        {"right", "0.25"},
        {"channel", "1"},
        textParameter("source", ""),
        resourceParameter("midiFile", ""),
        {"polyphony", "1"},
        {"lowPitch", "0 nn"},
        {"highPitch", "127 nn"},
        textParameter("script", ""),
        booleanParameter("shared", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeMidiInOutputDocument(
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("midi", std::move(typeId), std::move(parameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "midi", "audioIn"));
    document.graph.connections.push_back(connection("midi", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("midi", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeMappedMidiMapperChain(
    std::size_t mapperCount,
    std::size_t finalPolyphony = 8)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    std::string previousNode = "osc";
    std::string previousPort = "audioOut";
    for (std::size_t index = 0; index < mapperCount; ++index)
    {
        const auto mapperId = "midi" + std::to_string(index);
        const auto polyphony = index + 1 == mapperCount
            ? finalPolyphony
            : 8U;
        document.graph.nodes.push_back(node(
            mapperId,
            "talas.midiMapper",
            midiMapperParameters({
                textParameter("map", "gate"),
                {"channel", "0"},
                {"polyphony", std::to_string(polyphony)},
            })));
        document.graph.connections.push_back(connection(
            previousNode,
            previousPort,
            mapperId,
            "audioIn"));
        previousNode = mapperId;
        previousPort = "leftOut";
    }
    if (mapperCount == 0)
    {
        document.graph.connections.push_back(connection(
            "osc", "audioOut", "out", "audioIn"));
    }
    else
    {
        document.graph.connections.push_back(connection(
            previousNode, "leftOut", "out", "audioIn"));
        document.graph.connections.push_back(connection(
            previousNode, "rightOut", "out", "rightIn"));
    }
    return document;
}

talas::project::ProjectDocument makePreparedMidiVoiceOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> midiParameters =
        midiVoiceParameters({{"polyphony", "2"}}))
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("midi", "talas.midiVoice", std::move(midiParameters)));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "!Pitch"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node(
        "env",
        "talas.adsr",
        {
            {"attackTime", "0.001 s"},
            {"decayTime", "0.001 s"},
            {"sustainLevel", "1.0"},
            {"releaseTime", "0.001 s"},
            choiceParameter("type", "linear"),
            {"scale", "1.0"},
            {"gate", "!KeyDown"},
            {"legato", "0.0"},
        }));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "midi", "audioIn"));
    document.graph.connections.push_back(connection("midi", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("midi", "rightOut", "out", "rightIn"));
    return document;
}

talas::runtime::RenderSettings midiVoiceCompileSettings()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.defaultMidiChannel = 3;
    settings.eventValues = {
        {"Pitch", 60.0, talas::expr::ValueDomain::NoteNumber},
        {"KeyDown", 0.0, talas::expr::ValueDomain::Scalar},
    };
    return settings;
}

std::vector<talas::project::ProjectParameterExpression> midiFileEchoParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"lowChannel", "1"},
        {"highChannel", "16"},
        resourceParameter("fileName", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> midiOutputControllerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"channel", "1"},
        {"controllerNumber", "64"},
        textParameter("value", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> midiOutputEventParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "440 hz"},
        {"amplitude", "0.5"},
        {"channel", "1"},
        {"gate", "0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> midiOutputEventInBytesParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("bytes", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> soundToGlobalControllerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("generatedEvent", ""),
        textParameter("value", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeNoAudioTerminalOutputDocument(
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("terminal", std::move(typeId), std::move(parameters)));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> feedbackLoopInputParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("connection", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeFeedbackLoopInputTerminalOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = feedbackLoopInputParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("terminal", "talas.feedbackLoopInput", std::move(parameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "terminal", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeQuadOscillatorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> quadParameters = {
        {"frequency", "1 hz"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("quad", "talas.quadOscillator", std::move(quadParameters)));
    document.graph.nodes.push_back(node("input", "talas.audioInput"));
    document.graph.connections.push_back(connection("input", "audioOut", "quad", "envelope"));
    document.graph.connections.push_back(connection("quad", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> sumOfSinesParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"onDuration", "1 s"},
        {"frequency0", "100 hz"},
        {"frequency1", "100 hz"},
        resourceParameter("analysis0", "Tables/Partials"),
        {"analysisFrequency0", "1 hz"},
        {"analysisFrames0", "1"},
        resourceParameter("analysis1", ""),
        {"analysisFrequency1", "1 hz"},
        {"analysisFrames1", "1"},
        {"dbMorph", "0"},
        {"pchMorph", "0"},
        {"nbrPartials", "3"},
        {"bankSize", "3"},
        {"timeIndex", "0"},
        {"gate", "1"},
        booleanParameter("loop", false),
        {"loopStart", "0"},
        {"loopEnd", "0"},
        booleanParameter("ctrlTime", false),
        {"envelope", "0.5"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeSumOfSinesOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sumParameters = sumOfSinesParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("sum", "talas.sumOfSines", std::move(sumParameters)));
    document.graph.connections.push_back(connection("sum", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> gaOscillatorsParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"timeIndex", "0.0"},
        resourceParameter("analysis0", "Tables/GA"),
        resourceParameter("analysis1", ""),
        {"envelope", "0.25"},
        {"frequency", "1 hz"},
        {"morph", "0.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeGaOscillatorsOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> gaParameters = gaOscillatorsParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("ga", "talas.gaOscillators", std::move(gaParameters)));
    document.graph.connections.push_back(connection("ga", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> oscillatorBankParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrOscillators", "2"},
        {"bankSize", "2"},
        resourceParameter("wavetable", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> sosOscillatorsParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrOscillators", "2"},
        {"firstOscillator", "1"},
        resourceParameter("wavetable", ""),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> filterBankParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrFilters", "2"},
        {"bankSize", "2"},
        {"bandwidth", "10"},
        // The hertz Bandwidth these fixtures were written for.
        choiceParameter("bandwidthUnits", "legacyHertz"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> filterBankElementParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"first", "1"},
        {"count", "2"},
        {"bandwidth", "10"},
        // The hertz Bandwidth these fixtures were written for.
        choiceParameter("bandwidthUnits", "legacyHertz"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> formantBankParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "1 hz"},
        {"nbrFormants", "2"},
        {"bankSize", "2"},
        resourceParameter("impulseResponse", ""),
        {"nbrImpulses", "1"},
        booleanParameter("allowDC", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> formantBankOscillatorParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "1 hz"},
        {"firstFormant", "1"},
        {"nbrFormants", "2"},
        resourceParameter("formantImpulse", ""),
        {"nbrImpulses", "1"},
        booleanParameter("allowFormantAtDC", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> scaleVocoderParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"timeConstant", "10 ms"},
        {"nbrBands", "2"},
        {"bankSize", "2"},
        {"tonic", "60 nn"},
        textParameter("intervals", ""),
        {"sideLevel", "0"},
        {"inputLevel", "1"},
        {"inBandwidth", "1"},
        {"sideBandwidth", "1"},
        {"tone", "0"},
        {"rolloff", "0"},
        {"gain", "1"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> tunableVocoderParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"timeConstant", "10 ms"},
        {"nbrBands", "2"},
        {"bankSize", "2"},
        booleanParameter("logSpacing", false),
        {"analysisFreq", "100 hz"},
        {"synthesisFreq", "100 hz"},
        {"analysisLevel", "0"},
        {"synthesisLevel", "1"},
        {"analysisSpacing", "1"},
        {"synthesisSpacing", "1"},
        {"analysisBW", "1"},
        {"synthesisBW", "1"},
        {"tone", "0"},
        {"rolloff", "0"},
        {"gain", "1"},
        // These fixtures space bands by multiples of the base frequency.
        choiceParameter("spacingUnits", "legacyRelative"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> vocoderParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"timeConstant", "10 ms"},
        {"nbrBands", "2"},
        {"bankSize", "2"},
        {"inputLevel", "1"},
        {"sideLevel", "0"},
        {"lowCF", "100 hz"},
        {"highCF", "1000 hz"},
        {"inFreq", "1"},
        {"sideFreq", "1"},
        {"bw", "1"},
        booleanParameter("pitch", false),
        {"loCutoff", "20 hz"},
        {"hiCutoff", "2000 hz"},
        {"rolloff", "0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> vocoderChannelBankParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"timeConstant", "10 ms"},
        {"first", "1"},
        {"count", "2"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> contextFreeGrammarParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("rewriteRules", ""),
        textParameter("startExpression", ""),
        {"seed", "0"},
        {"maxRewrites", "0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> limeInterpreterParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("fileName", ""),
        textParameter("script", ""),
        {"left", "0.25"},
        {"right", "-0.5"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeContextFreeGrammarOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = contextFreeGrammarParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("grammar", "talas.contextFreeGrammar", std::move(parameters)));
    document.graph.connections.push_back(connection("grammar", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeLimeInterpreterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = limeInterpreterParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("lime", "talas.limeInterpreter", std::move(parameters)));
    document.graph.connections.push_back(connection("lime", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("lime", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> feedbackLoopOutputParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("connection", ""),
        {"delay", "0 s"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeFeedbackLoopOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = feedbackLoopOutputParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("feedback", "talas.feedbackLoopOutput", std::move(parameters)));
    document.graph.connections.push_back(connection("feedback", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePairedFeedbackLoopDocument(
    std::string connectionId = "loopA",
    std::string delay = "0 s")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("sink", "talas.feedbackLoopInput", feedbackLoopInputParameters({
        textParameter("connection", connectionId),
    })));
    document.graph.nodes.push_back(node("mix", "talas.mixer"));
    document.graph.nodes.push_back(node("feedback", "talas.feedbackLoopOutput", feedbackLoopOutputParameters({
        textParameter("connection", connectionId),
        {"delay", std::move(delay)},
    })));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(connection("feedback", "audioOut", "mix", "in2"));
    document.graph.connections.push_back(connection("mix", "audioOut", "sink", "audioIn"));
    document.graph.connections.push_back(connection("mix", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> scriptParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("script", ""),
        {"left", "0.5"},
        {"right", "-0.25"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeScriptOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = scriptParameters(),
    bool connectInput = true)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("script", "talas.script", std::move(parameters)));
    if (connectInput)
    {
        document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
        document.graph.connections.push_back(connection("osc", "audioOut", "script", "audioIn"));
    }
    document.graph.connections.push_back(connection("script", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("script", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeParameterTransformerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = {textParameter("transformation", "")})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("transform", "talas.parameterTransformer", std::move(parameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "transform", "audioIn"));
    document.graph.connections.push_back(connection("transform", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFftOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = {{"length", "1"}, booleanParameter("inverse", false)})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("fft", "talas.fft", std::move(parameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "fft", "audioIn"));
    document.graph.connections.push_back(connection("fft", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePackedFftRoundTripDocument(
    std::string length = "64")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "inverse",
        "talas.fft",
        {{"length", length}, booleanParameter("inverse", true), choiceParameter("packing", "legacyPacked")}));
    document.graph.nodes.push_back(node(
        "forward",
        "talas.fft",
        {{"length", std::move(length)}, booleanParameter("inverse", false), choiceParameter("packing", "legacyPacked")}));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "forward", "audioIn"));
    document.graph.connections.push_back(connection(
        "forward", "audioOut", "inverse", "audioIn"));
    document.graph.connections.push_back(connection(
        "inverse", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeSerialPackedFftRoundTripDocument(
    std::size_t pairCount,
    std::string length = "64")
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "osc",
        "talas.oscillator",
        {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    auto sourceNodeId = std::string{"osc"};
    for (std::size_t pair = 0; pair < pairCount; ++pair)
    {
        const auto forward = "forward" + std::to_string(pair);
        const auto inverse = "inverse" + std::to_string(pair);
        document.graph.nodes.push_back(node(
            forward,
            "talas.fft",
            {{"length", length}, booleanParameter("inverse", false), choiceParameter("packing", "legacyPacked")}));
        document.graph.nodes.push_back(node(
            inverse,
            "talas.fft",
            {{"length", length}, booleanParameter("inverse", true), choiceParameter("packing", "legacyPacked")}));
        document.graph.connections.push_back(connection(
            sourceNodeId, "audioOut", forward, "audioIn"));
        document.graph.connections.push_back(connection(
            forward, "audioOut", inverse, "audioIn"));
        sourceNodeId = inverse;
    }
    document.graph.connections.push_back(connection(
        sourceNodeId, "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> nativeStftParameters(
    std::string frameSize = "64",
    std::string hopSize = "64",
    std::string window = "rectangular")
{
    return {
        {"frameSize", std::move(frameSize)},
        {"hopSize", std::move(hopSize)},
        choiceParameter("window", std::move(window)),
    };
}

talas::project::ProjectDocument makeNativeStftOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> forwardParameters =
        nativeStftParameters(),
    std::vector<talas::project::ProjectParameterExpression> inverseParameters =
        nativeStftParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "inverse", "talas.inverseFftV1", std::move(inverseParameters)));
    document.graph.nodes.push_back(node(
        "forward", "talas.forwardFftV1", std::move(forwardParameters)));
    document.graph.nodes.push_back(node(
        "osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "forward", "audioIn"));
    document.graph.connections.push_back(connection(
        "forward", "spectrumOut", "inverse", "spectrumIn"));
    document.graph.connections.push_back(connection(
        "inverse", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> syntheticSpectrumFromArrayParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrPartials", "2"},
        booleanParameter("logScale", false),
        booleanParameter("sendBandwidths", true),
        {"envelope", "0.5"},
        textParameter("amplitudes", "1.0 0.25"),
        textParameter("frequencies", "100 200"),
        textParameter("bandwidths", "10 20"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeOscillatorBankOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> bankParameters = oscillatorBankParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("bank", "talas.oscillatorBank", std::move(bankParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection("bank", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeSosOscillatorsOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sosParameters = sosOscillatorsParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}));
    document.graph.nodes.push_back(node("sos", "talas.sosOscillators", std::move(sosParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "sos", "spectrum"));
    document.graph.connections.push_back(connection("cascade", "audioOut", "sos", "cascadeInput"));
    document.graph.connections.push_back(connection("sos", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeLowPassGateOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"control", "0.5"},
        {"closedFrequency", "10 hz"},
        {"openFrequency", "400 hz"},
        {"attackTime", "0.01 s"},
        {"releaseTime", "0.05 s"},
        {"curve", "2"},
        {"scale", "1"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "input", "talas.oscillator", {
            {"frequency", "50 hz"}, {"amplitude", "0.5"},
        }));
    document.graph.nodes.push_back(node(
        "gate", "talas.lowPassGate", std::move(parameters)));
    document.graph.connections.push_back(connection(
        "input", "audioOut", "gate", "audioIn"));
    document.graph.connections.push_back(connection(
        "gate", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFilterBankOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> bankParameters = filterBankParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "100 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("bank", "talas.filterBank", std::move(bankParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection("input", "audioOut", "bank", "audioIn"));
    document.graph.connections.push_back(connection("bank", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFilterBankElementOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> elementParameters = filterBankElementParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "100 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}));
    document.graph.nodes.push_back(node("element", "talas.filterBankElement", std::move(elementParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "element", "spectrum"));
    document.graph.connections.push_back(connection("input", "audioOut", "element", "audioIn"));
    document.graph.connections.push_back(connection("cascade", "audioOut", "element", "cascadeInput"));
    document.graph.connections.push_back(connection("element", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFormantBankOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> bankParameters = formantBankParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("bank", "talas.formantBank", std::move(bankParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "bank", "spectrum"));
    document.graph.connections.push_back(connection("bank", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFormantBankOscillatorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters = formantBankOscillatorParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}));
    document.graph.nodes.push_back(node("formant", "talas.formantBankOscillator", std::move(oscillatorParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "formant", "spectrum"));
    document.graph.connections.push_back(connection("cascade", "audioOut", "formant", "cascadeInput"));
    document.graph.connections.push_back(connection("formant", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFormantBankSerializedPairOutputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum", "talas.syntheticSpectrumFromArray",
        syntheticSpectrumFromArrayParameters({booleanParameter("sendBandwidths", false)})));
    document.graph.nodes.push_back(node("bank", "talas.formantBank", formantBankParameters()));
    document.graph.connections.push_back(connection("spectrum", "amplitudeOut", "bank", "amplitudeIn"));
    document.graph.connections.push_back(connection("spectrum", "frequencyOut", "bank", "frequencyIn"));
    document.graph.connections.push_back(connection("bank", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFormantBankOscillatorSerializedPairOutputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node(
        "spectrum", "talas.syntheticSpectrumFromArray",
        syntheticSpectrumFromArrayParameters({booleanParameter("sendBandwidths", false)})));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0"}}));
    document.graph.nodes.push_back(node(
        "formant", "talas.formantBankOscillator", formantBankOscillatorParameters()));
    document.graph.connections.push_back(connection("spectrum", "amplitudeOut", "formant", "amplitudeIn"));
    document.graph.connections.push_back(connection("spectrum", "frequencyOut", "formant", "frequencyIn"));
    document.graph.connections.push_back(connection("cascade", "audioOut", "formant", "cascadeInput"));
    document.graph.connections.push_back(connection("formant", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeVocoderOutputDocument(
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> parameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("side", "talas.oscillator", {{"frequency", "2 hz"}, {"amplitude", "0.0"}}));
    document.graph.nodes.push_back(node("vocoder", std::move(typeId), std::move(parameters)));
    document.graph.connections.push_back(connection("input", "audioOut", "vocoder", "audioIn"));
    document.graph.connections.push_back(connection("side", "audioOut", "vocoder", "sideChain"));
    document.graph.connections.push_back(connection("vocoder", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeVocoderChannelBankOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> parameters = vocoderChannelBankParameters(),
    std::vector<talas::project::ProjectParameterExpression> inputSpectrumParameters = syntheticSpectrumFromArrayParameters(),
    std::vector<talas::project::ProjectParameterExpression> sideSpectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.25"}}));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("side", "talas.oscillator", {{"frequency", "2 hz"}, {"amplitude", "0.0"}}));
    document.graph.nodes.push_back(node("inputParams", "talas.syntheticSpectrumFromArray", std::move(inputSpectrumParameters)));
    document.graph.nodes.push_back(node("sideParams", "talas.syntheticSpectrumFromArray", std::move(sideSpectrumParameters)));
    document.graph.nodes.push_back(node("bank", "talas.vocoderChannelBank", std::move(parameters)));
    document.graph.connections.push_back(connection("cascade", "audioOut", "bank", "cascadeInput"));
    document.graph.connections.push_back(connection("input", "audioOut", "bank", "audioIn"));
    document.graph.connections.push_back(connection("side", "audioOut", "bank", "sideChain"));
    document.graph.connections.push_back(connection("inputParams", "spectrumOut", "bank", "inputParameters"));
    document.graph.connections.push_back(connection("sideParams", "spectrumOut", "bank", "sideChainParameters"));
    document.graph.connections.push_back(connection("bank", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> syntheticSpectrumFromSoundsParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrPartials", "2"},
        booleanParameter("logScale", false),
        booleanParameter("includesBandwidths", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> spectrumTrackSelectorParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"track", "2"},
        booleanParameter("amp", true),
        booleanParameter("freq", true),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> spectrumInRamParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "100 hz"},
        {"level", "0.5"},
        {"timeIndex", "0"},
        resourceParameter("analysis", "Analysis/RAM"),
        {"analysisFrames", "1"},
        {"nbrPartials", "2"},
        {"firstPartial", "1"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> spectrumOnDiskParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "100 hz"},
        {"level", "0.5"},
        {"rateScale", "1"},
        {"trigger", "0"},
        resourceParameter("fileName", "Analysis/Disk"),
        {"analysisFrames", "1"},
        {"nbrPartials", "2"},
        {"firstPartial", "1"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeSpectrumInRamTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sourceParameters = spectrumInRamParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters({{"track", "1"}}))
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("source", "talas.spectrumInRam", std::move(sourceParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("source", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumOnDiskTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sourceParameters = spectrumOnDiskParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters({{"track", "1"}}))
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("source", "talas.spectrumOnDisk", std::move(sourceParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("source", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> spectrumFrequencyScaleParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"scale", "0.5"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> spectralShapeParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "100 hz"},
        {"spacing", "50 hz"},
        {"nbrPartials", "3"},
        resourceParameter("wavetable", "Tables/SpectralShape"),
        {"scale", "2.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> spectrumModifierParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        booleanParameter("select", true),
        {"loTrack", "1"},
        {"hiTrack", "2"},
        {"loFreq", "0 hz"},
        {"hiFreq", "1000 hz"},
        {"freqHysteresis", "0 hz"},
        {"freqScale", "0.5"},
        {"freqOffset", "10 hz"},
        {"loAmp", "0.0"},
        {"hiAmp", "1.0"},
        {"ampHysteresis", "0.0"},
        {"ampScale", "2.0"},
        {"ampOffset", "0.25"},
        {"probability", "1.0"},
        {"seed", "0.0"},
        booleanParameter("hearAll", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeSyntheticSpectrumTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromSoundsParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("amps", "talas.audioInput"));
    document.graph.nodes.push_back(node("freqs", "talas.audioInput", {booleanParameter("channel1", false), booleanParameter("channel2", true)}));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromSounds", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("amps", "audioOut", "spectrum", "amplitudes"));
    document.graph.connections.push_back(connection("freqs", "audioOut", "spectrum", "frequenciesAndBandwidths"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> liveSpectralAnalysisParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"lowestAnalyzedFreq", "3000 hz"},
        {"ampScale", "0.5"},
        {"freqScale", "1.0"},
        textParameter("response", ""),
        booleanParameter("harmonic", false),
        {"unpitchedOnly", "0"},
        {"unpitchedThreshold", "0"},
        {"trackedHarmonic", "1"},
        {"lowFreq", "1000 hz"},
        {"highFreq", "12000 hz"},
        {"initFreq", "3000 hz"},
        booleanParameter("fundamentalOnly", true),
        // The legacy peaks these fixtures were written for.
        choiceParameter("tracks", "legacyPeaks"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeLiveSpectralAnalysisTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> analysisParameters = liveSpectralAnalysisParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters({{"track", "1"}}))
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("input", "talas.audioInput"));
    document.graph.nodes.push_back(node("spectrum", "talas.liveSpectralAnalysis", std::move(analysisParameters)));
    document.graph.nodes.push_back(node("bridge", "talas.partialSpectrumLegacyHoldV1"));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("input", "audioOut", "spectrum", "audioIn"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "bridge", "spectrumIn"));
    document.graph.connections.push_back(connection("bridge", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument
makeLatencyPrefixedLiveSpectralAnalysisOutputDocument()
{
    auto document =
        makeLiveSpectralAnalysisTrackSelectorOutputDocument();
    document.graph.nodes.push_back(node(
        "latencyPrefix",
        "talas.fft",
        {{"length", "64"}, booleanParameter("inverse", false)}));
    document.graph.connections.erase(
        std::ranges::find_if(
            document.graph.connections,
            [](const talas::core::Connection& candidate) {
                return candidate.fromNode == "input"
                    && candidate.toNode == "spectrum";
            }));
    document.graph.connections.push_back(connection(
        "input", "audioOut", "latencyPrefix", "audioIn"));
    document.graph.connections.push_back(connection(
        "latencyPrefix", "audioOut", "spectrum", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeSpectralShapeTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> shapeParameters = spectralShapeParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("shape", "talas.spectralShape", std::move(shapeParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("shape", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumModifierTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters(),
    std::vector<talas::project::ProjectParameterExpression> modifierParameters = spectrumModifierParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("modifier", "talas.spectrumModifier", std::move(modifierParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "modifier", "spectrumIn"));
    document.graph.connections.push_back(connection("modifier", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumFrequencyScaleTrackSelectorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters(),
    std::vector<talas::project::ProjectParameterExpression> scaleParameters = spectrumFrequencyScaleParameters(),
    std::vector<talas::project::ProjectParameterExpression> selectorParameters = spectrumTrackSelectorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("scale", "talas.spectrumFrequencyScale", std::move(scaleParameters)));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector", std::move(selectorParameters)));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "scale", "spectrumIn"));
    document.graph.connections.push_back(connection("scale", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumLogToLinearOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("linear", "talas.spectrumLogToLinear"));
    document.graph.nodes.push_back(node("selector", "talas.spectrumTrackSelector"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "linear", "spectrumIn"));
    document.graph.connections.push_back(connection("linear", "spectrumOut", "selector", "spectrumIn"));
    document.graph.connections.push_back(connection("selector", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("selector", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumFundamentalOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("fundamental", "talas.spectrumFundamental"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "fundamental", "spectrumIn"));
    document.graph.connections.push_back(connection("fundamental", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("fundamental", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSpectrumVoicedUnvoicedOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = syntheticSpectrumFromArrayParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("voiced", "talas.spectrumVoicedUnvoiced"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "voiced", "spectrumIn"));
    document.graph.connections.push_back(connection("voiced", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("voiced", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSampleOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sampleParameters = {
        {"frequency", "440 hz"},
        {"gate", "1.0"},
        resourceParameter("sample", "Samples/Hit"),
        booleanParameter("setLoop", false),
        {"loopStart", "0.0"},
        {"loopEnd", "1.0"},
        booleanParameter("loopFade", false),
        {"start", "0.0"},
        {"end", "1.0"},
        booleanParameter("fromMemoryWriter", false),
        {"attackTime", "0 s"},
        {"releaseTime", "0 s"},
        {"scale", "0.75"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("sample", "talas.sample", std::move(sampleParameters)));
    document.graph.connections.push_back(connection("sample", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> diskPlayerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("fileName", "Samples/DiskFile"),
        {"filePosition", "250 ms"},
        {"duration", "500 ms"},
        {"rateScale", "1.0"},
        {"trigger", "1.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeDiskPlayerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> diskParameters = diskPlayerParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("disk", "talas.diskPlayer", std::move(diskParameters)));
    document.graph.connections.push_back(connection("disk", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeDiskCacheOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> cacheParameters = {
        resourceParameter("fileName", "Caches/Offline"),
        booleanParameter("record", false),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("cache", "talas.diskCache", std::move(cacheParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "cache", "audioIn"));
    document.graph.connections.push_back(connection("cache", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> diskRecorderParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("fileName", "Recordings/Capture"),
        textParameter("format", "AIFF"),
        textParameter("wordSize", "24"),
        choiceParameter("channels", "mono"),
        {"trigger", "1.0"},
        {"captureDuration", "0 s"},
        booleanParameter("gated", false),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeDiskRecorderTerminalDocument(
    std::vector<talas::project::ProjectParameterExpression> recorderParameters = diskRecorderParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("recorder", "talas.diskRecorder", std::move(recorderParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "recorder", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> memoryWriterParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"captureDuration", "1 s"},
        booleanParameter("global", false),
        booleanParameter("cyclic", false),
        resourceParameter("recordingName", "Memory/Capture"),
        booleanParameter("silent", false),
        {"trigger", "1.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeMemoryWriterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> writerParameters = memoryWriterParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("writer", "talas.memoryWriter", std::move(writerParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "writer", "audioIn"));
    document.graph.connections.push_back(connection("writer", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> sampleCloudParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("sample", "Samples/Cloud"),
        resourceParameter("grainEnv", "Envelopes/Flat"),
        {"maxGrains", "4"},
        {"amplitude", "0.5"},
        {"density", "1.0"},
        {"grainDur", "1 s"},
        {"grainDurJitter", "0.0"},
        {"pan", "0.5"},
        {"panJitter", "0.0"},
        {"seed", "0.0"},
        booleanParameter("fromMemoryWriter", false),
        {"timeIndex", "-1.0"},
        {"timeIndexJitter", "0.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeSampleCloudOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> cloudParameters = sampleCloudParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("cloud", "talas.sampleCloud", std::move(cloudParameters)));
    document.graph.connections.push_back(connection("cloud", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> grainCloudParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("waveform", "Tables/GrainWave"),
        resourceParameter("grainEnv", "Tables/GrainEnv"),
        {"maxGrains", "4"},
        {"amplitude", "0.5"},
        {"density", "1.0"},
        {"grainDur", "1 s"},
        {"grainDurJitter", "0.0"},
        {"cyclesPerGrain", "1.0"},
        {"frequency", "1 hz"},
        {"freqJitter", "0.0"},
        {"pan", "0.5"},
        {"panJitter", "0.0"},
        {"seed", "0.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeGrainCloudOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> cloudParameters = grainCloudParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("cloud", "talas.grainCloud", std::move(cloudParameters)));
    document.graph.connections.push_back(connection("cloud", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("cloud", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> cloudBankParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"nbrTracks", "2"},
        {"grainsPerTrack", "1"},
        {"bankSize", "2"},
        resourceParameter("grainWave", "Tables/CloudWave"),
        resourceParameter("grainEnvelope", "Tables/CloudEnv"),
        {"grainDur", "1 s"},
        {"cyclesPerGrain", "1"},
        {"seed", "0"},
        choiceParameter("interpolation", "linear"),
        // The regular grains these fixtures were written for.
        choiceParameter("grainStarts", "legacyRegular"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> cloudBankElementParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"first", "1"},
        {"count", "2"},
        resourceParameter("grainWave", "Tables/CloudWave"),
        resourceParameter("grainEnvelope", "Tables/CloudEnv"),
        {"grainDur", "1 s"},
        {"cyclesPerGrain", "1"},
        {"maxGrains", "4"},
        {"seed", "0"},
        {"initialDelay", "0 s"},
        choiceParameter("interpolation", "linear"),
        // The regular grains these fixtures were written for.
        choiceParameter("grainStarts", "legacyRegular"),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> cloudBankSpectrumParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    auto parameters = syntheticSpectrumFromArrayParameters({
        {"nbrPartials", "2"},
        {"envelope", "1.0"},
        textParameter("amplitudes", "1.0 0.5"),
        textParameter("frequencies", "1.0 1.0"),
        textParameter("bandwidths", "0.0 0.0"),
    });

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeCloudBankOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> cloudParameters = cloudBankParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = cloudBankSpectrumParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("trigger", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node("pan", "talas.constant", {{"value", "0.5"}}));
    document.graph.nodes.push_back(node("freqMod", "talas.constant", {{"value", "0"}}));
    document.graph.nodes.push_back(node("grainDurMod", "talas.constant", {{"value", "0"}}));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("cloud", "talas.cloudBank", std::move(cloudParameters)));
    document.graph.connections.push_back(connection("trigger", "value", "cloud", "trigger"));
    document.graph.connections.push_back(connection("pan", "value", "cloud", "pan"));
    document.graph.connections.push_back(connection("freqMod", "value", "cloud", "freqMod"));
    document.graph.connections.push_back(connection("grainDurMod", "value", "cloud", "grainDurMod"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "cloud", "spectrumSource"));
    document.graph.connections.push_back(connection("cloud", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("cloud", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeCloudBankElementOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> elementParameters = cloudBankElementParameters(),
    std::vector<talas::project::ProjectParameterExpression> spectrumParameters = cloudBankSpectrumParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("cascade", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "0.25"}}));
    document.graph.nodes.push_back(node("trigger", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node("pan", "talas.constant", {{"value", "0.5"}}));
    document.graph.nodes.push_back(node("freqMod", "talas.constant", {{"value", "0"}}));
    document.graph.nodes.push_back(node("grainDurMod", "talas.constant", {{"value", "0"}}));
    document.graph.nodes.push_back(node("spectrum", "talas.syntheticSpectrumFromArray", std::move(spectrumParameters)));
    document.graph.nodes.push_back(node("cloud", "talas.cloudBankElement", std::move(elementParameters)));
    document.graph.connections.push_back(connection("cascade", "audioOut", "cloud", "cascadeInput"));
    document.graph.connections.push_back(connection("trigger", "value", "cloud", "trigger"));
    document.graph.connections.push_back(connection("pan", "value", "cloud", "pan"));
    document.graph.connections.push_back(connection("freqMod", "value", "cloud", "freqMod"));
    document.graph.connections.push_back(connection("grainDurMod", "value", "cloud", "grainDurMod"));
    document.graph.connections.push_back(connection("spectrum", "spectrumOut", "cloud", "spectrumSource"));
    document.graph.connections.push_back(connection("cloud", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("cloud", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> multifileDiskPlayerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("fileNames", "Samples/A Samples/B"),
        {"rateScale", "2.0"},
        {"trigger", "1.0"},
        booleanParameter("gated", false),
        {"index", "1"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeMultifileDiskPlayerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> diskParameters = multifileDiskPlayerParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("multiDisk", "talas.multifileDiskPlayer", std::move(diskParameters)));
    document.graph.connections.push_back(connection("multiDisk", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> samplesFromDiskSingleStepParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        resourceParameter("fileName", "Samples/DiskStep"),
        {"filePosition", "1"},
        {"trigger", "1.0"},
        {"gate", "1.0"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeSamplesFromDiskSingleStepOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> diskParameters = samplesFromDiskSingleStepParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("disk", "talas.samplesFromDiskSingleStep", std::move(diskParameters)));
    document.graph.connections.push_back(connection("disk", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> multisampleParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "0 hz"},
        {"gate", "1.0"},
        textParameter("samples", "Samples/Hit Samples/Alt"),
        {"index", "1"},
        booleanParameter("loop", false),
        {"loopStart", "0.0"},
        {"loopEnd", "1.0"},
        {"attackTime", "0 s"},
        {"releaseTime", "0 s"},
        {"scale", "0.5"},
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeMultisampleOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> multiParameters = multisampleParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("multi", "talas.multisample", std::move(multiParameters)));
    document.graph.connections.push_back(connection("multi", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> keyMappedMultisampleParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"frequency", "440 hz"},
        {"gate", "1.0"},
        {"velocity", "0.5"},
        resourceParameter("firstSample", "Samples/Root"),
        {"loFreq", "220 hz"},
        {"hiFreq", "880 hz"},
        choiceParameter("mapping", "byPitchRange"),
        {"attackTime", "0 s"},
        {"releaseTime", "0 s"},
        {"scale", "0.25"},
        booleanParameter("loop", false),
        booleanParameter("fromDisk", false),
        booleanParameter("noTransposition", true),
    };

    for (auto& overrideParameter : overrides)
    {
        const auto found = std::ranges::find_if(parameters, [&](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == overrideParameter.parameterId;
        });
        if (found == parameters.end())
            parameters.push_back(std::move(overrideParameter));
        else
            *found = std::move(overrideParameter);
    }

    return parameters;
}

talas::project::ProjectDocument makeKeyMappedMultisampleOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> keymapParameters = keyMappedMultisampleParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("keymap", "talas.keyMappedMultisample", std::move(keymapParameters)));
    document.graph.connections.push_back(connection("keymap", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeAveragingLowPassFilterOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> filterParameters = {
        {"cutoff", "1 hz"},
        resourceParameter("wavetable", "Private"),
        {"scale", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("avg", "talas.averagingLowPassFilter", std::move(filterParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "avg", "audioIn"));
    document.graph.connections.push_back(connection("avg", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeNoiseOutputDocument(std::vector<talas::project::ProjectParameterExpression> noiseParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("noise", "talas.noise", std::move(noiseParameters)));
    document.graph.connections.push_back(connection("noise", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeInputOutputCharacteristicDocument(
    std::vector<talas::project::ProjectParameterExpression> characteristicParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mapper", "talas.inputOutputCharacteristic", std::move(characteristicParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "mapper", "audioIn"));
    document.graph.connections.push_back(connection("mapper", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeWaveshaperOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> waveshaperParameters = {
        choiceParameter("shapeFrom", "polynomial"),
        textParameter("coefficients", "0 1"),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("shape", "talas.waveshaper", std::move(waveshaperParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "shape", "audioIn"));
    document.graph.connections.push_back(connection("shape", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeMultiplyingWaveshaperOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> waveshaperParameters = {
        {"scale", "0.5"},
        resourceParameter("wavetable", "Tables/Dynamics"),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("shape", "talas.multiplyingWaveshaper", std::move(waveshaperParameters)));
    document.graph.nodes.push_back(node("index", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "2.0"}}));
    document.graph.connections.push_back(connection("index", "audioOut", "shape", "nonlinearInput"));
    document.graph.connections.push_back(connection("input", "audioOut", "shape", "audioIn"));
    document.graph.connections.push_back(connection("shape", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeIteratedWaveshaperOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> waveshaperParameters = {
        {"iterations", "2"},
        {"scale", "0.5"},
        resourceParameter("wavetable", "Tables/Dynamics"),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("shape", "talas.iteratedWaveshaper", std::move(waveshaperParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "shape", "audioIn"));
    document.graph.connections.push_back(connection("shape", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeLevelOutputDocument(std::vector<talas::project::ProjectParameterExpression> levelParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("level", "talas.level", std::move(levelParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "level", "audioIn"));
    document.graph.connections.push_back(connection("level", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("level", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeCrossfadeOutputDocument(std::vector<talas::project::ProjectParameterExpression> crossfadeParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("xfade", "talas.crossfade", std::move(crossfadeParameters)));
    document.graph.nodes.push_back(node("snd1", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("snd2", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "2.0"}}));
    document.graph.connections.push_back(connection("snd1", "audioOut", "xfade", "snd1"));
    document.graph.connections.push_back(connection("snd2", "audioOut", "xfade", "snd2"));
    document.graph.connections.push_back(connection("xfade", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("xfade", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeDynamicRangeControllerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> controllerParameters = {
        choiceParameter("type", "compressor"),
        {"ratio", "2.0"},
        {"threshold", "0.5"},
        {"attackTime", "0.001 ms"},
        {"releaseTime", "0.001 ms"},
        {"delay", "0 s"},
        {"gain", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("drc", "talas.dynamicRangeController", std::move(controllerParameters)));
    document.graph.nodes.push_back(node("side", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("input", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("side", "audioOut", "drc", "sideChain"));
    document.graph.connections.push_back(connection("input", "audioOut", "drc", "audioIn"));
    document.graph.connections.push_back(connection("drc", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeReverbSectionOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> reverbParameters = {
        choiceParameter("type", "comb"),
        {"scale", "0.75"},
        {"decayTime", "1 s"},
        {"delay", "250 ms"},
        {"delayScale", "0.5"},
        resourceParameter("wavetable", "Private"),
        booleanParameter("prezero", true),
        choiceParameter("interpolation", "linear"),
        booleanParameter("smoothDelayChanges", true),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("reverb", "talas.reverbSection", std::move(reverbParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "reverb", "audioIn"));
    document.graph.connections.push_back(connection("reverb", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeChannelCrosserOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> crosserParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("crosser", "talas.channelCrosser", std::move(crosserParameters)));
    document.graph.nodes.push_back(node("left", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("right", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "2.0"}}));
    document.graph.connections.push_back(connection("left", "audioOut", "crosser", "audioIn"));
    document.graph.connections.push_back(connection("right", "audioOut", "crosser", "rightIn"));
    document.graph.connections.push_back(connection("crosser", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("crosser", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeChannelJoinOutputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("join", "talas.channelJoin"));
    document.graph.nodes.push_back(node("left", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("right", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "2.0"}}));
    document.graph.connections.push_back(connection("left", "audioOut", "join", "left"));
    document.graph.connections.push_back(connection("right", "audioOut", "join", "right"));
    document.graph.connections.push_back(connection("join", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("join", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeChannellerOutputDocument(std::vector<talas::project::ProjectParameterExpression> channellerParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("channeller", "talas.channeller", std::move(channellerParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "channeller", "audioIn"));
    document.graph.connections.push_back(connection("channeller", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("channeller", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSampleAndHoldOutputDocument(std::vector<talas::project::ProjectParameterExpression> holdParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("ramp", "talas.linearRamp", {{"duration", "1 s"}, {"start", "1"}, {"end", "5"}}));
    document.graph.nodes.push_back(node("hold", "talas.sampleAndHold", std::move(holdParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("ramp", "value", "hold", "value"));
    document.graph.connections.push_back(connection("hold", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> frequencyTrackerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"minFrequency", "1 hz"},
        {"maxFrequency", "8 hz"},
        {"confidence", "0.5"},
        {"scale", "0.25"},
        {"detectors", "10"},
        {"emphasis", "0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeFrequencyTrackerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> trackerParameters = frequencyTrackerParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("source", "talas.oscillator", {{"frequency", "4 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("carrier", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("tracker", "talas.frequencyTracker", std::move(trackerParameters)));
    document.graph.connections.push_back(connection("source", "audioOut", "tracker", "audioIn"));
    document.graph.connections.push_back(connection("carrier", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("tracker", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePulseTrainOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> pulseParameters = {
        {"period", "1 s"},
        booleanParameter("variableDutyCycle", false),
        {"dutyCycle", "0.5"},
        {"gate", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("pulse", "talas.pulseTrain", std::move(pulseParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("pulse", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makePulseGeneratorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> pulseParameters = {
        {"frequency", "1 hz"},
        textParameter("modulation", ""),
        {"maxMI", "0"},
        choiceParameter("interpolation", "linear"),
        {"envelope", "1.0"},
        {"dutyCycle", "0.25"},
        {"reset", "0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("pulse", "talas.pulseGenerator", std::move(pulseParameters)));
    document.graph.connections.push_back(connection("pulse", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> multislopeFunctionGeneratorParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("timePoints", "{0 s} {1 s} {2 s}"),
        textParameter("slopes", "{-1} {1}"),
        {"level", "1.0"},
        {"gate", "1.0"},
        {"rate", "1.0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> multisegmentEnvelopeParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("durations", "{1 s} {1 s}"),
        textParameter("breakPoints", "{0} {1} {0.5}"),
        {"startLoop", "0.0"},
        {"endLoop", "0.0"},
        {"level", "1.0"},
        {"gate", "1.0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

std::vector<talas::project::ProjectParameterExpression> graphicalEnvelopeParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("envelope", "{0 s} {0} {1 s} {1} {2 s} {0.5}"),
        {"level", "1.0"},
        {"gate", "1.0"},
        {"rate", "1.0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeMultislopeFunctionGeneratorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> multiParameters = multislopeFunctionGeneratorParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("multi", "talas.multislopeFunctionGenerator", std::move(multiParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("multi", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeMultisegmentEnvelopeOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> envelopeParameters = multisegmentEnvelopeParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("env", "talas.multisegmentEnvelope", std::move(envelopeParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeGraphicalEnvelopeOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> envelopeParameters = graphicalEnvelopeParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("env", "talas.graphicalEnvelope", std::move(envelopeParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeFunctionGeneratorOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> generatorParameters = {
        {"trigger", "1.0"},
        {"onDuration", "1 s"},
        resourceParameter("wavetable", "Tables/Envelope"),
        booleanParameter("fromMemoryWriter", false),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("fg", "talas.functionGenerator", std::move(generatorParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("fg", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeChopperOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> chopperParameters = {
        {"grainDuration", "1 s"},
        {"interGrainDelay", "0.5 s"},
        resourceParameter("wavetable", "Tables/Grain"),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("chop", "talas.chopper", std::move(chopperParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "chop", "audioIn"));
    document.graph.connections.push_back(connection("chop", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeArOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> envelopeParameters = {
        {"attackTime", "0.5 s"},
        {"releaseTime", "0.25 s"},
        choiceParameter("type", "linear"),
        {"scale", "1.0"},
        {"gate", "1.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("env", "talas.ar", std::move(envelopeParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeAdsrOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> envelopeParameters = {
        {"attackTime", "0.5 s"},
        {"decayTime", "0.5 s"},
        {"sustainLevel", "0.25"},
        {"releaseTime", "0.25 s"},
        choiceParameter("type", "linear"),
        {"scale", "1.0"},
        {"gate", "1.0"},
        {"legato", "0.0"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("env", "talas.adsr", std::move(envelopeParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("env", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTriggeredTableReadOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> tableParameters = {
        {"trigger", "1.0"},
        resourceParameter("wavetable", "Tables/Steps"),
        {"gate", "1.0"},
        booleanParameter("ignoreLoops", true),
        booleanParameter("fromMemoryWriter", false),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("table", "talas.triggeredTableRead", std::move(tableParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("table", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTriggeredSampleAndHoldOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> holdParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("ramp", "talas.linearRamp", {{"duration", "1 s"}, {"start", "1"}, {"end", "5"}}));
    document.graph.nodes.push_back(node("hold", "talas.triggeredSampleAndHold", std::move(holdParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("ramp", "value", "hold", "value"));
    document.graph.connections.push_back(connection("hold", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTimeGateOutputDocument(
    std::string typeId,
    std::vector<talas::project::ProjectParameterExpression> gateParameters)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("gate", std::move(typeId), std::move(gateParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "gate", "audioIn"));
    document.graph.connections.push_back(connection("gate", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTimeControlOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> timeParameters = {
        {"rate", "0.5"},
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("time", "talas.timeControl", std::move(timeParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "time", "audioIn"));
    document.graph.connections.push_back(connection("time", "audioOut", "out", "audioIn"));
    return document;
}

talas::project::ProjectDocument makeTimeOffsetOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> offsetParameters = {
        {"offset", "0.25 s"},
        booleanParameter("reverse", false),
        booleanParameter("retrograde", false),
    })
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("offset", "talas.timeOffset", std::move(offsetParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "offset", "audioIn"));
    document.graph.connections.push_back(connection("offset", "audioOut", "out", "audioIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> warpedTimeIndexParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("idealTimePoints0", "{0 s} {2 s}"),
        textParameter("idealTimePoints1", "{0 s} {1 s}"),
        textParameter("currentTimePoints", "{0 s} {4 s}"),
        {"trigger", "1.0"},
        {"rate", "1.0"},
        {"morph", "0.0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeWarpedTimeIndexOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> indexParameters = warpedTimeIndexParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "0.25 hz"}, {"amplitude", "1.0"}}));
    document.graph.nodes.push_back(node("index", "talas.warpedTimeIndex", std::move(indexParameters)));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("index", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));
    return document;
}

void testPanGraphCompilesWithStereoOutputBuffers()
{
    const auto document = makePanStereoOutputDocument({{"pan", "0.5"}, {"scale", "1.0"}, choiceParameter("type", "power")});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "pan graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "pan left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "pan", "pan left output buffer should point to pan");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "pan left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "pan right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "pan right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "pan", "pan right output buffer should point to pan");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "pan right output buffer should point to rightOut");
}

void testLevelGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(
        makeLevelOutputDocument({{"left", "0.25"}, {"right", "0.75"}}),
        makeRegistry(),
        {});

    require(result.ok(), "level graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "level left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "level", "level left output buffer should point to level");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "level left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "level right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "level right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "level", "level right output buffer should point to level");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "level right output buffer should point to rightOut");

    const auto& level = requireStep(plan, "level");
    const auto& audioIn = requireInputBinding(level, "audioIn");
    require(audioIn.sourceNodeId == "osc", "level.audioIn should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "level.audioIn should bind to oscillator audio output");
}

void testCrossfadeGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(
        makeCrossfadeOutputDocument({{"pan", "0.5"}, {"scale", "1.0"}, {"fade", "0.25"}, choiceParameter("type", "linear")}),
        makeRegistry(),
        {});

    require(result.ok(), "crossfade graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "crossfade left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "xfade", "crossfade left output buffer should point to xfade");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "crossfade left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "crossfade right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "crossfade right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "xfade", "crossfade right output buffer should point to xfade");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "crossfade right output buffer should point to rightOut");

    const auto& crossfade = requireStep(plan, "xfade");
    require(requireInputBinding(crossfade, "snd1").sourceNodeId == "snd1", "crossfade.snd1 should bind to first sound");
    require(requireInputBinding(crossfade, "snd2").sourceNodeId == "snd2", "crossfade.snd2 should bind to second sound");
}

void testDynamicRangeControllerGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeDynamicRangeControllerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "dynamic range controller graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "dynamic range controller output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "drc", "dynamic range controller output buffer should point to drc");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "dynamic range controller output buffer should point to audioOut");

    const auto& drc = requireStep(plan, "drc");
    requireChoiceParameter(drc, "type", "compressor");
    requireNear(requireParameter(drc, "ratio").value, 2.0, 1.0e-12, "dynamic range ratio should compile");
    requireNear(requireParameter(drc, "threshold").value, 0.5, 1.0e-12, "dynamic range threshold should compile");
    requireNear(requireParameter(drc, "attackTime").value, 1.0e-6, 1.0e-15, "dynamic range attackTime should compile");
    requireNear(requireParameter(drc, "releaseTime").value, 1.0e-6, 1.0e-15, "dynamic range releaseTime should compile");
    requireNear(requireParameter(drc, "delay").value, 0.0, 1.0e-12, "dynamic range delay should compile");
    requireNear(requireParameter(drc, "gain").value, 1.0, 1.0e-12, "dynamic range gain should compile");

    const auto& sideChain = requireInputBinding(drc, "sideChain");
    require(sideChain.sourceNodeId == "side", "dynamic range sideChain should bind side oscillator");
    require(sideChain.sourcePortId == "audioOut", "dynamic range sideChain should bind oscillator audio output");
    require(sideChain.bufferIndex < plan.buffers.size(), "dynamic range sideChain buffer index should be valid");
    require(plan.buffers[sideChain.bufferIndex].nodeId == "side", "dynamic range sideChain buffer should point to side");

    const auto& audioIn = requireInputBinding(drc, "audioIn");
    require(audioIn.sourceNodeId == "input", "dynamic range audioIn should bind input oscillator");
    require(audioIn.sourcePortId == "audioOut", "dynamic range audioIn should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "dynamic range audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "input", "dynamic range audioIn buffer should point to input");
}

void testReverbSectionGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeReverbSectionOutputDocument(), makeRegistry(), {});

    require(result.ok(), "reverb section graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "reverb section output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "reverb", "reverb section output buffer should point to reverb");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "reverb section output buffer should point to audioOut");

    const auto& reverb = requireStep(plan, "reverb");
    require(
        reverb.offlineSupport == talas::core::ExecutionSupport::Supported
            && reverb.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && reverb.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && reverb.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && reverb.supportsReset
            && reverb.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && reverb.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{{0}}
            && reverb.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && reverb.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "C141 Reverb Section should compile with supported resettable zero-latency authority");
    requireChoiceParameter(reverb, "type", "comb");
    requireNear(requireParameter(reverb, "scale").value, 0.75, 1.0e-12, "reverb section scale should compile");
    requireNear(requireParameter(reverb, "decayTime").value, 1.0, 1.0e-12, "reverb section decayTime should compile");
    requireNear(requireParameter(reverb, "delay").value, 0.25, 1.0e-12, "reverb section delay should compile");
    requireNear(requireParameter(reverb, "delayScale").value, 0.5, 1.0e-12, "reverb section delayScale should compile");
    requireResourceParameter(reverb, "wavetable", "Private");
    requireBooleanParameter(reverb, "prezero", true);
    requireChoiceParameter(reverb, "interpolation", "linear");
    requireBooleanParameter(reverb, "smoothDelayChanges", true);
    require(requireInputBinding(reverb, "audioIn").sourceNodeId == "osc", "reverb section audioIn should bind oscillator");
}

void testReverbSectionGraphCompilesWithConnectedDelayScale()
{
    auto document = makeReverbSectionOutputDocument();
    document.graph.nodes.push_back(node("delayScale", "talas.constant", {{"value", "0.5"}}));
    document.graph.connections.push_back(connection("delayScale", "value", "reverb", "delayScale"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "reverb section graph with connected delayScale should compile");
    const auto& reverb = requireStep(result.graph->plan(), "reverb");
    const auto& delayScale = requireInputBinding(reverb, "delayScale");
    require(delayScale.sourceNodeId == "delayScale", "reverb section delayScale should bind constant");
    require(delayScale.sourcePortId == "value", "reverb section delayScale should bind constant value output");
}

void testReverbSectionConnectedControlsOwnInvalidDirectFallbacks()
{
    auto document = makeReverbSectionOutputDocument({
        choiceParameter("type", "comb"),
        {"scale", "1 / 0"},
        {"decayTime", "1 / 0"},
        {"delay", "1 / 0"},
        {"delayScale", "1 / 0"},
        resourceParameter("wavetable", "Private"),
        booleanParameter("prezero", true),
        choiceParameter("interpolation", "linear"),
        booleanParameter("smoothDelayChanges", true),
    });
    for (const auto& [nodeId, inputId, value] :
         std::array{
             std::tuple{"decay", "decayTime", "1"},
             std::tuple{"delay", "delay", "0.25"},
             std::tuple{"delayScale", "delayScale", "0.5"},
             std::tuple{"scale", "scale", "0.75"},
         })
    {
        document.graph.nodes.push_back(node(
            nodeId, "talas.constant", {{"value", value}}));
        document.graph.connections.push_back(connection(
            nodeId, "value", "reverb", inputId));
    }

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(),
        "C104 connected Reverb Section controls should own valid buffers independently of invalid hidden direct fallbacks: "
            + compileDiagnosticsSummary(result));
    const auto& reverb = requireStep(result.graph->plan(), "reverb");
    require(reverb.inputs.size() == 5
            && reverb.inputs[0].inputPortId == "audioIn"
            && reverb.inputs[1].inputPortId == "decayTime"
            && reverb.inputs[2].inputPortId == "delay"
            && reverb.inputs[3].inputPortId == "delayScale"
            && reverb.inputs[4].inputPortId == "scale",
        "C104 connected Reverb Section should preserve exact descriptor input order");
}

void testChannelCrosserGraphCompilesWithStereoInputAndOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeChannelCrosserOutputDocument(), makeRegistry(), {});

    require(result.ok(), "channel crosser graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "channel crosser left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "crosser", "channel crosser left output buffer should point to crosser");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "channel crosser left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "channel crosser right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "channel crosser right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "crosser", "channel crosser right output buffer should point to crosser");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "channel crosser right output buffer should point to rightOut");

    const auto& crosser = requireStep(plan, "crosser");
    const auto& audioIn = requireInputBinding(crosser, "audioIn");
    const auto& rightIn = requireInputBinding(crosser, "rightIn");
    require(audioIn.sourceNodeId == "left", "channel crosser audioIn should bind left source");
    require(audioIn.sourcePortId == "audioOut", "channel crosser audioIn should bind left source audio");
    require(rightIn.sourceNodeId == "right", "channel crosser rightIn should bind right source");
    require(rightIn.sourcePortId == "audioOut", "channel crosser rightIn should bind right source audio");
}

void testChannelJoinGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeChannelJoinOutputDocument(), makeRegistry(), {});

    require(result.ok(), "channel join graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "channel join left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "join", "channel join left output buffer should point to join");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "channel join left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "channel join right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "channel join right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "join", "channel join right output buffer should point to join");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "channel join right output buffer should point to rightOut");

    const auto& join = requireStep(plan, "join");
    require(requireInputBinding(join, "left").sourceNodeId == "left", "channel join left input should bind to left source");
    require(requireInputBinding(join, "right").sourceNodeId == "right", "channel join right input should bind to right source");
}

void testChannellerGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(
        makeChannellerOutputDocument({booleanParameter("leftChannel", true), booleanParameter("rightChannel", false)}),
        makeRegistry(),
        {});

    require(result.ok(), "channeller graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "channeller left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "channeller", "channeller left output buffer should point to channeller");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "channeller left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "channeller right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "channeller right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "channeller", "channeller right output buffer should point to channeller");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "channeller right output buffer should point to rightOut");

    const auto& channeller = requireStep(plan, "channeller");
    const auto& audioIn = requireInputBinding(channeller, "audioIn");
    require(audioIn.sourceNodeId == "osc", "channeller input should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "channeller input should bind to oscillator audio output");
}

void testChannellerGraphCompilesWithIndependentLiveSelectors()
{
    auto document = makeChannellerOutputDocument({
        booleanParameter("leftChannel", false),
        booleanParameter("rightChannel", true),
    });
    document.graph.nodes.push_back(node(
        "leftSelector", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "rightSelector", "talas.constant", {{"value", "0"}}));
    document.graph.connections.push_back(connection(
        "leftSelector", "value", "channeller", "leftChannel"));
    document.graph.connections.push_back(connection(
        "rightSelector", "value", "channeller", "rightChannel"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});

    require(result.ok(), "channeller live selectors should compile");
    const auto& channeller = requireStep(result.graph->plan(), "channeller");
    const auto& left = requireInputBinding(channeller, "leftChannel");
    const auto& right = requireInputBinding(channeller, "rightChannel");
    require(
        left.sourceNodeId == "leftSelector" && left.sourcePortId == "value",
        "left selector should preserve its exact Control producer");
    require(
        right.sourceNodeId == "rightSelector" && right.sourcePortId == "value",
        "right selector should preserve its exact Control producer");
    requireBooleanParameter(channeller, "leftChannel", false);
    requireBooleanParameter(channeller, "rightChannel", true);
}

void testFilterGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makeFilterOutputDocument({
        choiceParameter("type", "lowPass"),
        {"frequency", "500 hz"},
        {"q", "0.70710678118"},
        {"scale", "1.0"},
        {"feedback", "0.0"},
        {"order", "4"},
    });

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "filter output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "filter", "filter output buffer should point to filter");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "filter output buffer should point to audioOut");

    const auto& filter = requireStep(plan, "filter");
    const auto& audioIn = requireInputBinding(filter, "audioIn");
    require(audioIn.sourceNodeId == "osc", "filter.audioIn should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "filter.audioIn should bind to oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "filter.audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "filter.audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "filter.audioIn buffer should point to audioOut");
}

void testFirFilterGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makeFirFilterOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "fir filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "fir filter output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "fir", "fir filter output buffer should point to fir");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "fir filter output buffer should point to audioOut");

    const auto& filter = requireStep(plan, "fir");
    requireTextParameter(filter, "coefficients", "0.5 0.25");

    const auto& audioIn = requireInputBinding(filter, "audioIn");
    require(audioIn.sourceNodeId == "osc", "fir filter audioIn should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "fir filter audioIn should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "fir filter audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "fir filter audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "fir filter audioIn buffer should point to audioOut");
}

void testShelvingFilterGraphsCompileWithAudioOutputBuffers()
{
    const auto highResult = talas::runtime::compileProject(
        makeShelvingFilterOutputDocument("highShelf", "talas.highShelvingFilter"),
        makeRegistry(),
        {});
    require(highResult.ok(), "high shelving filter graph should compile");
    const auto& highPlan = highResult.graph->plan();
    require(highPlan.outputBufferIndex < highPlan.buffers.size(), "high shelving output buffer index should be valid");
    require(highPlan.buffers[highPlan.outputBufferIndex].nodeId == "highShelf", "high shelving output buffer should point to shelf node");
    require(highPlan.buffers[highPlan.outputBufferIndex].portId == "audioOut", "high shelving output buffer should point to audioOut");
    const auto& high = requireStep(highPlan, "highShelf");
    requireNear(requireParameter(high, "cutoffFreq").value, 500.0, 1.0e-9, "high shelving cutoff should compile");
    requireNear(requireParameter(high, "boostOrCut").value, 6.0, 1.0e-12, "high shelving boost should compile");
    requireNear(requireParameter(high, "scale").value, 1.0, 1.0e-12, "high shelving scale should compile");
    require(requireInputBinding(high, "audioIn").sourceNodeId == "osc", "high shelving audioIn should bind oscillator");

    const auto lowResult = talas::runtime::compileProject(
        makeShelvingFilterOutputDocument("lowShelf", "talas.lowShelvingFilter"),
        makeRegistry(),
        {});
    require(lowResult.ok(), "low shelving filter graph should compile");
    const auto& lowPlan = lowResult.graph->plan();
    require(lowPlan.outputBufferIndex < lowPlan.buffers.size(), "low shelving output buffer index should be valid");
    require(lowPlan.buffers[lowPlan.outputBufferIndex].nodeId == "lowShelf", "low shelving output buffer should point to shelf node");
    require(lowPlan.buffers[lowPlan.outputBufferIndex].portId == "audioOut", "low shelving output buffer should point to audioOut");
    const auto& low = requireStep(lowPlan, "lowShelf");
    requireNear(requireParameter(low, "cutoffFreq").value, 500.0, 1.0e-9, "low shelving cutoff should compile");
    requireNear(requireParameter(low, "boostOrCut").value, 6.0, 1.0e-12, "low shelving boost should compile");
    requireNear(requireParameter(low, "scale").value, 1.0, 1.0e-12, "low shelving scale should compile");
    require(requireInputBinding(low, "audioIn").sourceNodeId == "osc", "low shelving audioIn should bind oscillator");
}

void testPresenceFilterGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makePresenceFilterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "presence filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "presence filter output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "presence", "presence filter output buffer should point to presence");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "presence filter output buffer should point to audioOut");

    const auto& presence = requireStep(plan, "presence");
    requireNear(requireParameter(presence, "centerFreq").value, 500.0, 1.0e-9, "presence centerFreq should compile");
    requireNear(requireParameter(presence, "bandwidth").value, 200.0, 1.0e-9, "presence bandwidth should compile");
    requireNear(requireParameter(presence, "boostOrCut").value, 6.0, 1.0e-12, "presence boost should compile");
    requireNear(requireParameter(presence, "scale").value, 1.0, 1.0e-12, "presence scale should compile");
    require(requireInputBinding(presence, "audioIn").sourceNodeId == "osc", "presence audioIn should bind oscillator");
}

void testTwoFormantElementGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeTwoFormantElementOutputDocument(), makeRegistry(), {});

    require(result.ok(), "two formant element graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "two formant output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "formants", "two formant output buffer should point to formants");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "two formant output buffer should point to audioOut");

    const auto& formants = requireStep(plan, "formants");
    requireNear(requireParameter(formants, "formant1").value, 500.0, 1.0e-9, "two formant formant1 should compile");
    requireNear(requireParameter(formants, "bandwidth1").value, 100.0, 1.0e-9, "two formant bandwidth1 should compile");
    requireNear(requireParameter(formants, "scale1").value, 1.0, 1.0e-12, "two formant scale1 should compile");
    requireNear(requireParameter(formants, "formant2").value, 1500.0, 1.0e-9, "two formant formant2 should compile");
    requireNear(requireParameter(formants, "bandwidth2").value, 200.0, 1.0e-9, "two formant bandwidth2 should compile");
    requireNear(requireParameter(formants, "scale2").value, 0.5, 1.0e-12, "two formant scale2 should compile");
    require(requireInputBinding(formants, "audioIn").sourceNodeId == "osc", "two formant audioIn should bind oscillator");
}

void testTwoFormantVoiceElementGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeTwoFormantVoiceElementOutputDocument(), makeRegistry(), {});

    require(result.ok(), "two formant voice element graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "two formant voice output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "voice", "two formant voice output buffer should point to voice");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "two formant voice output buffer should point to audioOut");

    const auto& voice = requireStep(plan, "voice");
    require(voice.typeId == "talas.twoFormantVoiceElement", "two formant voice step should retain node type");
    require(voice.inputs.empty(), "two formant voice should not require inputs");
    requireNear(requireParameter(voice, "frequency").value, 200.0, 1.0e-9, "two formant voice frequency should compile");
    requireNear(requireParameter(voice, "formant1").value, 500.0, 1.0e-9, "two formant voice formant1 should compile");
    requireNear(requireParameter(voice, "bandwidth1").value, 100.0, 1.0e-9, "two formant voice bandwidth1 should compile");
    requireNear(requireParameter(voice, "scale1").value, 1.0, 1.0e-12, "two formant voice scale1 should compile");
    requireNear(requireParameter(voice, "formant2").value, 1500.0, 1.0e-9, "two formant voice formant2 should compile");
    requireNear(requireParameter(voice, "bandwidth2").value, 200.0, 1.0e-9, "two formant voice bandwidth2 should compile");
    requireNear(requireParameter(voice, "scale2").value, 0.5, 1.0e-12, "two formant voice scale2 should compile");
    requireNear(requireParameter(voice, "seed").value, 123.0, 1.0e-12, "two formant voice seed should compile");
}

void testDualParallelTwoPoleFilterGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeDualParallelTwoPoleFilterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "dual parallel two pole filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "dual parallel two pole output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "twoPole", "dual parallel two pole output buffer should point to filter");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "dual parallel two pole output buffer should point to audioOut");

    const auto& filter = requireStep(plan, "twoPole");
    requireTextParameter(filter, "pole1", "0.5 % 0.25");
    requireNear(requireParameter(filter, "scale1").value, 1.0, 1.0e-12, "dual parallel two pole scale1 should compile");
    requireTextParameter(filter, "pole2", "0.25 % -0.5");
    requireNear(requireParameter(filter, "scale2").value, 0.5, 1.0e-12, "dual parallel two pole scale2 should compile");
    require(requireInputBinding(filter, "audioIn").sourceNodeId == "osc", "dual parallel two pole audioIn should bind oscillator");
}

void testHarmonicResonatorGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeHarmonicResonatorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "harmonic resonator graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "harmonic resonator output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "resonator", "harmonic resonator output buffer should point to resonator");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "harmonic resonator output buffer should point to audioOut");

    const auto& resonator = requireStep(plan, "resonator");
    requireNear(requireParameter(resonator, "decayTime").value, 0.25, 1.0e-12, "harmonic resonator decayTime should compile");
    requireNear(requireParameter(resonator, "brightness").value, 1.0, 1.0e-12, "harmonic resonator brightness should compile");
    requireNear(requireParameter(resonator, "frequency").value, 4.0, 1.0e-12, "harmonic resonator frequency should compile");
    requireResourceParameter(resonator, "wavetable", "Private");
    requireBooleanParameter(resonator, "prezero", true);
    requireNear(requireParameter(resonator, "scale").value, 0.75, 1.0e-12, "harmonic resonator scale should compile");
    require(requireInputBinding(resonator, "audioIn").sourceNodeId == "osc", "harmonic resonator audioIn should bind oscillator");
    require(
        resonator.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && resonator.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && resonator.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && resonator.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && resonator.supportsReset
            && resonator.resolvedLatencyFrames == 0
            && !resonator.kernelKind.has_value()
            && !resonator.preparedIsolatedComponent,
        "harmonic resonator compile plan should publish exact C195 prepared authority");
}

void testReResonatorGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeReResonatorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "RE resonator graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "RE resonator output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "resonator", "RE resonator output buffer should point to resonator");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "RE resonator output buffer should point to audioOut");

    const auto& resonator = requireStep(plan, "resonator");
    require(resonator.typeId == "talas.reResonator", "RE resonator step should keep type id");
    requireResourceParameter(resonator, "wavetable", "Tables/RE");
    requireNear(requireParameter(resonator, "timeIndex").value, 0.0, 1.0e-12, "RE resonator timeIndex should compile");
    require(requireInputBinding(resonator, "audioIn").sourceNodeId == "osc", "RE resonator audioIn should bind oscillator");
}

void testGraphicEqGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeGraphicEqOutputDocument(), makeRegistry(), {});

    require(result.ok(), "graphic eq graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "graphic eq output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "eq", "graphic eq output buffer should point to eq");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "graphic eq output buffer should point to audioOut");

    const auto& eq = requireStep(plan, "eq");
    requireNear(requireParameter(eq, "cf250Hz").value, 1.0, 1.0e-12, "graphic eq cf250Hz should compile");
    requireNear(requireParameter(eq, "cf500Hz").value, 2.0, 1.0e-12, "graphic eq cf500Hz should compile");
    requireNear(requireParameter(eq, "cf1000Hz").value, 1.0, 1.0e-12, "graphic eq cf1000Hz should compile");
    requireNear(requireParameter(eq, "cf2000Hz").value, 1.0, 1.0e-12, "graphic eq cf2000Hz should compile");
    requireNear(requireParameter(eq, "cf4000Hz").value, 1.0, 1.0e-12, "graphic eq cf4000Hz should compile");
    requireNear(requireParameter(eq, "cf8000Hz").value, 1.0, 1.0e-12, "graphic eq cf8000Hz should compile");
    requireNear(requireParameter(eq, "cf16000Hz").value, 1.0, 1.0e-12, "graphic eq cf16000Hz should compile");
    require(requireInputBinding(eq, "audioIn").sourceNodeId == "osc", "graphic eq audioIn should bind oscillator");
}

void testVcfGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeVcfOutputDocument(), makeRegistry(), {});

    require(result.ok(), "vcf graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "vcf output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "vcf", "vcf output buffer should point to vcf");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "vcf output buffer should point to audioOut");

    const auto& vcf = requireStep(plan, "vcf");
    requireNear(requireParameter(vcf, "amplitude").value, 0.75, 1.0e-12, "vcf amplitude should compile");
    requireNear(requireParameter(vcf, "cutoff").value, 500.0, 1.0e-9, "vcf cutoff should compile");
    requireNear(requireParameter(vcf, "cutoffModulator").value, 0.0, 1.0e-12, "vcf cutoffModulator should compile");
    requireNear(requireParameter(vcf, "cutoffModRange").value, 100.0, 1.0e-9, "vcf cutoffModRange should compile");
    requireNear(requireParameter(vcf, "resonance").value, 0.25, 1.0e-12, "vcf resonance should compile");
    require(requireInputBinding(vcf, "audioIn").sourceNodeId == "osc", "vcf audioIn should bind oscillator");
}

void testSingleSideBandRmGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSingleSideBandRmOutputDocument(), makeRegistry(), {});

    require(result.ok(), "single side band rm graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "single side band rm output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "ssb", "single side band rm output buffer should point to ssb");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "single side band rm output buffer should point to audioOut");

    const auto& ssb = requireStep(plan, "ssb");
    requireNear(requireParameter(ssb, "frequency").value, 440.0, 1.0e-9, "single side band rm frequency should compile");
    requireNear(requireParameter(ssb, "freqScale").value, 2.0, 1.0e-12, "single side band rm freqScale should compile");
    require(requireInputBinding(ssb, "audioIn").sourceNodeId == "osc", "single side band rm audioIn should bind oscillator");
}

void testPhaseShiftBy90GraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makePhaseShiftBy90OutputDocument(), makeRegistry(), {});

    require(result.ok(), "phase shift by 90 graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "phase shift by 90 output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "phase", "phase shift by 90 output buffer should point to phase");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "phase shift by 90 output buffer should point to audioOut");

    const auto& phase = requireStep(plan, "phase");
    require(phase.typeId == "talas.phaseShiftBy90", "phase shift by 90 step should keep type id");
    requireNear(requireParameter(phase, "frequency").value, 440.0, 1.0e-9, "phase shift by 90 frequency should compile");
    require(requireInputBinding(phase, "audioIn").sourceNodeId == "osc", "phase shift by 90 audioIn should bind oscillator");
}

void testAnalysisFilterGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeAnalysisFilterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "analysis filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "analysis filter left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "analysis", "analysis filter left output buffer should point to analysis");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "analysis filter left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "analysis filter right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "analysis filter right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "analysis", "analysis filter right output buffer should point to analysis");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "analysis filter right output buffer should point to rightOut");

    const auto& analysis = requireStep(plan, "analysis");
    require(analysis.typeId == "talas.analysisFilter", "analysis filter step should retain node type");
    requireNear(requireParameter(analysis, "fundamental").value, 100.0, 1.0e-9, "analysis filter fundamental should compile");
    requireNear(requireParameter(analysis, "harmonic").value, 2.0, 1.0e-12, "analysis filter harmonic should compile");
    const auto& audioIn = requireInputBinding(analysis, "audioIn");
    require(audioIn.sourceNodeId == "osc", "analysis filter audioIn should bind oscillator");
    require(audioIn.sourcePortId == "audioOut", "analysis filter audioIn should bind oscillator audio output");
}

void testAudioInputGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeAudioInputOutputDocument(), makeRegistry(), {});

    require(result.ok(), "audio input graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "audio input output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "input", "audio input output buffer should point to input");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "audio input output buffer should point to audioOut");

    const auto& input = requireStep(plan, "input");
    require(input.typeId == "talas.audioInput", "audio input step should keep type id");
    requireBooleanParameter(input, "channel1", true);
    requireBooleanParameter(input, "channel2", false);
    requireBooleanParameter(input, "channel3", false);
    requireBooleanParameter(input, "channel4", false);
    requireBooleanParameter(input, "channel5", false);
    requireBooleanParameter(input, "channel6", false);
    requireBooleanParameter(input, "channel7", false);
    requireBooleanParameter(input, "channel8", false);
}

void testGenericSourceGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeGenericSourceOutputDocument(), makeRegistry(), {});

    require(result.ok(), "generic source graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "generic source left output buffer index should be valid");
    require(plan.rightOutputBufferIndex.has_value(), "generic source right output buffer index should be present");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "source", "generic source left output buffer should point to source");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "generic source left output buffer should point to leftOut");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "source", "generic source right output buffer should point to source");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "generic source right output buffer should point to rightOut");

    const auto& source = requireStep(plan, "source");
    requireTextParameter(source, "source", "live input");
    requireBooleanParameter(source, "leftChannel", true);
    requireBooleanParameter(source, "rightChannel", true);
    requireResourceParameter(source, "sample", "");
    requireBooleanParameter(source, "autoloop", false);
    requireNear(requireParameter(source, "trigger").value, 1.0, 1.0e-12, "generic source trigger should compile");
    requireNear(requireParameter(source, "attackTime").value, 0.0, 1.0e-12, "generic source attackTime should compile");
    requireNear(requireParameter(source, "releaseTime").value, 0.0, 1.0e-12, "generic source releaseTime should compile");
    requireNear(requireParameter(source, "scale").value, 0.5, 1.0e-12, "generic source scale should compile");
    requireNear(requireParameter(source, "frequency").value, 440.0, 1.0e-9, "generic source frequency should compile");
    require(
        source.parameters.size() == 10
            && source.parameters[0].id == "source"
            && source.parameters[1].id == "leftChannel"
            && source.parameters[2].id == "rightChannel"
            && source.parameters[3].id == "sample"
            && source.parameters[4].id == "autoloop"
            && source.parameters[5].id == "trigger"
            && source.parameters[6].id == "attackTime"
            && source.parameters[7].id == "releaseTime"
            && source.parameters[8].id == "scale"
            && source.parameters[9].id == "frequency"
            && source.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && source.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && source.supportsReset
            && !source.kernelKind.has_value()
            && source.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && source.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && source.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !source.preparedIsolatedComponent,
        "generic source compile plan should preserve exact C33 prepared authority");
}

void requireMidiInCompilePlan(const talas::runtime::CompileResult& result, std::string_view expectedTypeId)
{
    if (!result.ok())
    {
        std::string message = std::string(expectedTypeId) + " blank MIDI-In graph should compile; diagnostics:";
        for (const auto& diagnostic : result.diagnostics)
        {
            message += " [node=" + diagnostic.nodeId
                + " param=" + diagnostic.parameterId
                + " message=" + diagnostic.message + "]";
        }
        require(false, message);
    }
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), std::string(expectedTypeId) + " left output buffer index should be valid");
    require(plan.rightOutputBufferIndex.has_value(), std::string(expectedTypeId) + " right output index should be present");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "midi", std::string(expectedTypeId) + " left output should point to midi node");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", std::string(expectedTypeId) + " left output should point to leftOut");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "midi", std::string(expectedTypeId) + " right output should point to midi node");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", std::string(expectedTypeId) + " right output should point to rightOut");

    const auto& midi = requireStep(plan, "midi");
    require(midi.typeId == expectedTypeId, std::string(expectedTypeId) + " step should retain node type");
    if (expectedTypeId == "talas.midiMapper")
    {
        require(requireInputBinding(midi, "audioIn").sourceNodeId == "osc",
                "talas.midiMapper audioIn should bind oscillator");
        require(plan.midiVoices.empty(), "talas.midiMapper should not create prepared MIDI voice scopes");
    }
    else
    {
        require(std::ranges::none_of(midi.inputs, [](const talas::runtime::InputBinding& input) {
            return input.inputPortId == "audioIn";
        }), "talas.midiVoice should consume prepared voice roots instead of one global audioIn binding");
        require(plan.midiVoices.size() == 1, "talas.midiVoice should create one prepared voice plan");
    }
    requireNear(requireParameter(midi, "left").value, 0.5, 1.0e-12, std::string(expectedTypeId) + " left gain should compile");
    requireNear(requireParameter(midi, "right").value, 0.25, 1.0e-12, std::string(expectedTypeId) + " right gain should compile");
    requireNear(requireParameter(midi, "channel").value, 1.0, 1.0e-12, std::string(expectedTypeId) + " channel should compile");
    requireTextParameter(midi, "source", "");
    requireResourceParameter(midi, "midiFile", "");
    requireNear(requireParameter(midi, "polyphony").value, 1.0, 1.0e-12, std::string(expectedTypeId) + " polyphony should compile");
    requireNear(requireParameter(midi, "lowPitch").value, 0.0, 1.0e-12, std::string(expectedTypeId) + " lowPitch should compile");
    requireNear(requireParameter(midi, "highPitch").value, 127.0, 1.0e-12, std::string(expectedTypeId) + " highPitch should compile");
    requireTextParameter(midi, "script", "");
    requireBooleanParameter(midi, "shared", false);
}

void testBlankMidiInGraphsCompileWithStereoOutputBuffers()
{
    auto result = talas::runtime::compileProject(makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters()), makeRegistry(), {});
    requireMidiInCompilePlan(result, "talas.midiMapper");
    requireTextParameter(requireStep(result.graph->plan(), "midi"), "map", "");
    require(result.graph->plan().midiMappers.empty(),
        "blank MIDI Mapper should not claim prepared MIDI ownership");

    result = talas::runtime::compileProject(makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters()), makeRegistry(), {});
    requireMidiInCompilePlan(result, "talas.midiVoice");
}

void testMappedMidiMapperCompilesExactPreparedPlan()
{
    auto result = talas::runtime::compileProject(
        makeMidiInOutputDocument(
            "talas.midiMapper",
            midiMapperParameters({
                textParameter("map", "gate"),
                {"channel", "0"},
                {"polyphony", "4"},
                {"lowPitch", "36 nn"},
                {"highPitch", "84 nn"},
            })),
        makeRegistry(),
        {});
    require(result.ok(), "gate MIDI Mapper should compile");
    const auto& gatePlan = result.graph->plan();
    require(gatePlan.midiMappers.size() == 1,
        "gate MIDI Mapper should publish one prepared plan");
    const auto& gate = gatePlan.midiMappers.front();
    require(gate.ownerStepIndex < gatePlan.steps.size()
            && gatePlan.steps[gate.ownerStepIndex].nodeId == "midi"
            && gatePlan.steps[gate.ownerStepIndex].typeId == "talas.midiMapper",
        "prepared mapper should bind the exact owner step");
    require(gate.nodeId == "midi"
            && gate.scopeId
                == gatePlan.steps[gate.ownerStepIndex].scopeId,
        "prepared mapper should retain exact node and scope identity");
    require(gate.configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Gate
            && gate.configuration.channel == 0
            && gate.configuration.polyphony == 4
            && gate.configuration.lowPitch == 36
            && gate.configuration.highPitch == 84,
        "gate mapper should retain exact bounded configuration");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument(
            "talas.midiMapper",
            midiMapperParameters({
                textParameter("map", " velocity "),
                {"channel", "16"},
                {"polyphony", "128"},
            })),
        makeRegistry(),
        {});
    require(result.ok(), "velocity MIDI Mapper should compile");
    const auto& velocity = result.graph->plan().midiMappers.front();
    require(velocity.configuration.mode
                == talas::runtime::PreparedMidiMapperMode::Velocity
            && velocity.configuration.channel == 16
            && velocity.configuration.polyphony == 128,
        "velocity mapper should retain exact maximum configuration");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument(
            "talas.midiMapper",
            midiMapperParameters({
                textParameter(
                    "map",
                    std::string(
                        talas::runtime::maximumPreparedMidiMapperMapBytes,
                        ' ')),
            })),
        makeRegistry(),
        {});
    require(result.ok()
            && result.graph->plan().midiMappers.empty(),
        "exact-bound blank map should preserve legacy passthrough");
}

void testMappedMidiMapperAggregateLimitsFailClosed()
{
    auto exact = talas::runtime::compileProject(
        makeMappedMidiMapperChain(
            talas::runtime::maximumPreparedMidiMapperInstances,
            8),
        makeRegistry(),
        {});
    require(exact.ok(),
        "exact mapper instance and voice limits should compile");
    require(exact.graph->plan().midiMappers.size()
                == talas::runtime::maximumPreparedMidiMapperInstances,
        "exact mapper fixture should publish every bounded plan");
    std::size_t exactVoiceCount = 0;
    for (const auto& mapper : exact.graph->plan().midiMappers)
        exactVoiceCount += mapper.configuration.polyphony;
    require(exactVoiceCount
                == talas::runtime::maximumPreparedMidiMapperVoices,
        "exact mapper fixture should consume the aggregate voice authority");

    auto tooManyMappers = talas::runtime::compileProject(
        makeMappedMidiMapperChain(
            talas::runtime::maximumPreparedMidiMapperInstances + 1,
            1),
        makeRegistry(),
        {});
    requireCompileFailure(
        tooManyMappers,
        DiagnosticCode::GraphValidationFailed,
        "midi64",
        "map");

    auto tooManyVoices = talas::runtime::compileProject(
        makeMappedMidiMapperChain(
            talas::runtime::maximumPreparedMidiMapperInstances,
            9),
        makeRegistry(),
        {});
    requireCompileFailure(
        tooManyVoices,
        DiagnosticCode::GraphValidationFailed,
        "midi63",
        "polyphony");
}

void testMidiVoiceCompilesIndependentPreparedVoiceScopes()
{
    auto settings = midiVoiceCompileSettings();
    auto document = makePreparedMidiVoiceOutputDocument(midiVoiceParameters({
        {"channel", "0"},
        {"polyphony", "2"},
        {"lowPitch", "24.4 nn"},
        {"highPitch", "96.6 nn"},
    }));
    talas::project::ProjectVariableDeclaration voiceGain;
    voiceGain.variableId = "voiceGain";
    voiceGain.displayName = "Voice Gain";
    voiceGain.initialValue = 0.25;
    voiceGain.snapshotEligible = false;
    voiceGain.owner.kind =
        talas::project::ProjectVariableOwnerKind::MidiVoice;
    voiceGain.owner.nodeId = "midi";
    document.variableLibrary.variables.push_back(voiceGain);
    auto voiceBias = voiceGain;
    voiceBias.variableId = "voiceBias";
    voiceBias.displayName = "Voice Bias";
    voiceBias.initialValue = 0.0;
    document.variableLibrary.variables.push_back(voiceBias);
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
    };
    talas::project::ProjectSoundVariableDeclaration projectSound;
    projectSound.variableId = "projectSound";
    projectSound.displayName = "Project Sound";
    projectSound.candidateDefinitionIds = {"toneA", "toneB"};
    projectSound.initialSelectedOrdinal = 1;
    talas::project::ProjectSoundVariableDeclaration voiceSound = projectSound;
    voiceSound.variableId = "voiceSound";
    voiceSound.displayName = "Voice Sound";
    voiceSound.initialSelectedOrdinal = 0;
    voiceSound.snapshotEligible = false;
    voiceSound.owner.kind = talas::project::ProjectVariableOwnerKind::MidiVoice;
    voiceSound.owner.nodeId = "midi";
    document.soundVariableLibrary.variables = {
        std::move(projectSound),
        std::move(voiceSound),
    };
    const auto oscillator = std::ranges::find(
        document.graph.nodes,
        std::string{"osc"},
        &talas::project::ProjectNode::instanceId);
    require(oscillator != document.graph.nodes.end(),
        "voice-variable fixture should expose its oscillator");
    const auto amplitude = std::ranges::find(
        oscillator->parameters,
        std::string{"amplitude"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(amplitude != oscillator->parameters.end(),
        "voice-variable fixture should expose oscillator amplitude");
    amplitude->expression = "@voiceGain";
    const auto result = talas::runtime::compileProject(
        document,
        makeRegistry(),
        settings);

    require(result.ok(), "prepared MIDI voice graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.midiVoices.size() == 1, "prepared graph should contain one MIDI voice plan");
    const auto& midiPlan = plan.midiVoices.front();
    require(midiPlan.ownerStepIndex < plan.steps.size(), "MIDI voice owner step index should be valid");
    require(plan.steps[midiPlan.ownerStepIndex].nodeId == "midi", "MIDI voice owner should reference midi step");
    require(plan.steps[midiPlan.ownerStepIndex].scopeId == talas::runtime::globalRuntimeScope,
            "MIDI voice owner should remain in global scope");
    require(midiPlan.configuration.channel == 3, "channel zero should resolve through default MIDI channel");
    require(midiPlan.configuration.lowPitch == 24, "low pitch should round to nearest MIDI note");
    require(midiPlan.configuration.highPitch == 97, "high pitch should round to nearest MIDI note");
    require(midiPlan.configuration.polyphony == 2, "compiled allocator should retain polyphony");
    require(midiPlan.configuration.retriggerDelayFrames == 240, "compiled allocator should use five millisecond retrigger delay");
    require(midiPlan.voices.size() == 2, "polyphony two should compile two prepared voices");
    require(
        midiPlan.ownerNodeId == "midi"
            && midiPlan.ownerDefinitionId.empty()
            && midiPlan.ownerInstancePath.empty(),
        "MIDI voice plan should retain exact root structural owner identity");

    std::vector<std::size_t> rootBuffers;
    for (std::size_t voiceIndex = 0; voiceIndex < midiPlan.voices.size(); ++voiceIndex)
    {
        const auto& voice = midiPlan.voices[voiceIndex];
        require(voice.scopeId == static_cast<talas::runtime::RuntimeScopeId>(voiceIndex + 1),
                "voice scopes should be assigned in voice-number order");
        require(
            voice.voiceOrdinal == voiceIndex
                && voice.firstVariableCellIndex == voiceIndex * 2
                && voice.variableCellCount == 2 &&
                voice.firstSoundVariableCellIndex == voiceIndex + 1 &&
                voice.soundVariableCellCount == 1,
            "each voice plan should publish exact deterministic contiguous "
            "scalar and Sound-variable ranges"
        );
        require(voice.stepCount == 3, "each voice should contain oscillator, envelope, and VCA");
        require(voice.firstStepIndex + voice.stepCount <= plan.steps.size(), "voice step range should be valid");
        for (std::size_t offset = 0; offset < voice.stepCount; ++offset)
        {
            const auto& step = plan.steps[voice.firstStepIndex + offset];
            require(step.scopeId == voice.scopeId, "every voice step should retain its scope");
            require(step.nodeId == "osc" || step.nodeId == "env" || step.nodeId == "vca",
                    "voice range should contain only the audioIn ancestor closure");
            if (step.typeId == "talas.adsr")
            {
                require(step.offlineSupport
                            == talas::core::ExecutionSupport::Subset
                        && step.offlineBinding
                            == talas::core::ExecutionBinding::Kernel
                        && step.preparedRealtimeSupport
                            == talas::core::ExecutionSupport::Subset
                        && step.preparedRealtimeBinding
                            == talas::core::ExecutionBinding::Kernel,
                    "per-voice ADSR should retain legacy both-Subset kernel compatibility without promoted polyphonic authority");
            }
            if (step.typeId == "talas.oscillator")
            {
                require(step.offlineSupport
                            == talas::core::ExecutionSupport::Subset
                        && step.offlineBinding
                            == talas::core::ExecutionBinding::Kernel
                        && step.preparedRealtimeSupport
                            == talas::core::ExecutionSupport::Subset
                        && step.preparedRealtimeBinding
                            == talas::core::ExecutionBinding::Kernel,
                    "per-voice Oscillator should retain legacy both-Subset kernel compatibility without promoted polyphonic authority");
            }
            for (const auto& input : step.inputs)
                require(input.sourceScopeId == voice.scopeId, "voice-local inputs should bind within their scope");
        }

        require(voice.audioOutputBufferIndex < plan.buffers.size(), "voice root buffer index should be valid");
        const auto& root = plan.buffers[voice.audioOutputBufferIndex];
        require(root.scopeId == voice.scopeId, "voice root buffer should retain voice scope");
        require(root.nodeId == "vca" && root.portId == "audioOut",
                "voice root buffer should reference the complete upstream graph output");
        rootBuffers.push_back(voice.audioOutputBufferIndex);
    }
    require(rootBuffers[0] != rootBuffers[1], "prepared voices should own distinct root buffers");

    const auto& soundVariables = result.graph->preparedSoundVariables();
    require(
        soundVariables.cells.size() == 3 &&
            soundVariables.cells.front().identity.variableId == "projectSound" &&
            soundVariables.cells.front().initialSelectedOrdinal == 1,
        "compiled graph should retain one project and one exact Sound cell "
        "per prepared voice"
    );
    for (std::size_t voiceIndex = 0; voiceIndex < midiPlan.voices.size(); ++voiceIndex)
    {
        const auto cellIndex = voiceIndex + 1;
        require(
            soundVariables.cells[cellIndex].identity.ownerKind ==
                    talas::project::ProjectVariableOwnerKind::MidiVoice &&
                soundVariables.cells[cellIndex].identity.ownerNodeId == "midi" &&
                soundVariables.cells[cellIndex].identity.voiceOrdinal ==
                    std::optional<std::uint16_t>{static_cast<std::uint16_t>(voiceIndex)} &&
                soundVariables.cells[cellIndex].initialSelectedOrdinal == 0,
            "each prepared voice should retain one exact Sound-variable "
            "identity and initial selection"
        );
    }
    talas::runtime::RealtimeRenderSession session(*result.graph);
    require(session.ready(), std::string(session.error()));
    require(!session.gateTargetIndex("env").has_value(),
        "per-voice Subset ADSR should not mint one ambiguous promoted typed-gate target");
    const auto soundPublication = session.preparedSoundVariableValues();
    require(
        soundPublication.publicationValid && soundPublication.values.size() == 3 &&
            soundPublication.values.front().selectedDefinitionId == "toneB" &&
            soundPublication.values[1].selectedDefinitionId == "toneA" &&
            soundPublication.values[2].selectedDefinitionId == "toneA",
        "realtime publication should expose exact initial definition IDs "
        "without reading project bytes"
    );

    const auto& variables = result.graph->preparedVariables();
    require(variables.cells.size() == 4,
        "two MIDI-voice declarations should materialize once per prepared slot");
    for (std::size_t voiceIndex = 0;
         voiceIndex < midiPlan.voices.size();
         ++voiceIndex)
    {
        const auto& voice = midiPlan.voices[voiceIndex];
        const auto cell = std::ranges::find_if(
            variables.cells,
            [&](const auto& candidate) {
                return candidate.scopeOrdinal == voice.scopeId
                    && candidate.identity.ownerKind
                        == talas::project::ProjectVariableOwnerKind::MidiVoice
                    && candidate.identity.ownerNodeId == "midi"
                    && candidate.identity.variableId == "voiceGain"
                    && candidate.identity.voiceOrdinal
                        == std::optional<std::uint16_t>{
                            static_cast<std::uint16_t>(voiceIndex)};
            });
        require(cell != variables.cells.end(),
            "each prepared voice scope should own one exact variable cell");
        const auto oscillatorStep = std::ranges::find_if(
            plan.steps.begin()
                + static_cast<std::ptrdiff_t>(voice.firstStepIndex),
            plan.steps.begin()
                + static_cast<std::ptrdiff_t>(
                    voice.firstStepIndex + voice.stepCount),
            [](const auto& step) { return step.nodeId == "osc"; });
        require(oscillatorStep
                    != plan.steps.begin()
                        + static_cast<std::ptrdiff_t>(
                            voice.firstStepIndex + voice.stepCount),
            "each prepared voice should retain its oscillator clone");
        const auto gain = std::ranges::find(
            oscillatorStep->parameters,
            std::string{"amplitude"},
            &talas::runtime::ParameterValue::id);
        require(gain != oscillatorStep->parameters.end()
                && gain->variableScopeId == voice.scopeId
                && gain->variableCellIndices.size() == 1
                && gain->variableCellIndices.front()
                    == static_cast<std::uint32_t>(
                        std::distance(variables.cells.begin(), cell)),
            "each cloned expression should bind its own exact voice-variable cell");
    }

    require(std::ranges::none_of(plan.steps, [](const talas::runtime::CompileStep& step) {
        return step.scopeId == talas::runtime::globalRuntimeScope
            && (step.nodeId == "osc" || step.nodeId == "env" || step.nodeId == "vca");
    }), "voice template nodes should not execute in global scope");
    require(plan.steps[midiPlan.ownerStepIndex].inputs.end()
                == std::ranges::find_if(plan.steps[midiPlan.ownerStepIndex].inputs, [](const talas::runtime::InputBinding& input) {
                    return input.inputPortId == "audioIn";
                }),
            "MIDI voice owner should mix prepared voice roots instead of binding one global audioIn buffer");

    talas::project::SnapshotValueEntry polyphonyOverlay;
    polyphonyOverlay.target.kind =
        talas::project::SnapshotTargetKind::NodeParameter;
    polyphonyOverlay.target.nodeId = "midi";
    polyphonyOverlay.target.parameterId = "polyphony";
    polyphonyOverlay.value.number = 3.0;
    const auto overlay = talas::runtime::compileProjectPerformanceOverlay(
        document,
        makeRegistry(),
        settings,
        std::span<const talas::project::SnapshotValueEntry>(
            &polyphonyOverlay,
            1));
    require(
        overlay.ok()
            && overlay.graph->plan().midiVoices.front().voices.size()
                == 3
            && overlay.graph->preparedVariables().cells.size() == 6,
        "snapshot-overlaid polyphony should determine the final exact voice scopes and cells"
            + compileDiagnosticsSummary(overlay));
}

void testAdsrMixedExactAndPerVoiceSubsetAuthority()
{
    auto document = makePreparedMidiVoiceOutputDocument(
        midiVoiceParameters({{"polyphony", "2"}}));
    const auto leftOutput = std::ranges::find_if(
        document.graph.connections,
        [](const auto& graphConnection) {
            return graphConnection.fromNode == "midi"
                && graphConnection.fromPort == "leftOut"
                && graphConnection.toNode == "out"
                && graphConnection.toPort == "audioIn";
        });
    require(leftOutput != document.graph.connections.end(),
        "C159 mixed fixture should expose the MIDI left terminal binding");
    leftOutput->toNode = "rootMix";
    leftOutput->toPort = "in1";
    document.graph.nodes.push_back(node("rootMix", "talas.mixer"));
    document.graph.nodes.push_back(node(
        "rootCarrier", "talas.oscillator",
        {{"frequency", "0.25 hz"}, {"amplitude", "0.25"}}));
    document.graph.nodes.push_back(node(
        "rootEnv",
        "talas.adsr",
        {
            {"attackTime", "1 s"},
            {"decayTime", "1 s"},
            {"sustainLevel", "0.5"},
            {"releaseTime", "1 s"},
            choiceParameter("type", "linear"),
            {"scale", "1"},
            {"gate", "0"},
            {"legato", "0"},
        }));
    document.graph.connections.push_back(connection(
        "rootEnv", "value", "rootCarrier", "frequency"));
    document.graph.connections.push_back(connection(
        "rootCarrier", "audioOut", "rootMix", "in2"));
    document.graph.connections.push_back(connection(
        "rootMix", "audioOut", "out", "audioIn"));
    const auto compiled = talas::runtime::compileProject(
        document, makeRegistry(), midiVoiceCompileSettings());
    require(compiled.ok(),
        "C159 mixed exact/per-voice Subset ADSR graph should compile;"
            + compileDiagnosticsSummary(compiled));
    const auto& plan = compiled.graph->plan();
    const auto root = std::ranges::find(
        plan.steps, std::string_view{"rootEnv"},
        &talas::runtime::CompileStep::nodeId);
    require(root != plan.steps.end()
            && root->scopeId == talas::runtime::globalRuntimeScope
            && root->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && root->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C159 mixed graph should retain one exact root Supported ADSR");
    const auto perVoiceAdsrCount = std::ranges::count_if(
        plan.steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.adsr"
                && step.nodeId == "env"
                && step.scopeId != talas::runtime::globalRuntimeScope
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.offlineBinding
                    == talas::core::ExecutionBinding::Kernel
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::Kernel;
        });
    require(perVoiceAdsrCount == 2,
        "C159 mixed graph should retain two contextual per-voice both-Subset ADSR kernels");
    const auto rootOscillator = std::ranges::find(
        plan.steps, std::string_view{"rootCarrier"},
        &talas::runtime::CompileStep::nodeId);
    require(rootOscillator != plan.steps.end()
            && rootOscillator->scopeId
                == talas::runtime::globalRuntimeScope
            && rootOscillator->offlineSupport
                == talas::core::ExecutionSupport::Supported
            && rootOscillator->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C160 mixed graph should retain one exact root Supported Oscillator");
    const auto perVoiceOscillatorCount = std::ranges::count_if(
        plan.steps,
        [](const talas::runtime::CompileStep& step) {
            return step.typeId == "talas.oscillator"
                && step.nodeId == "osc"
                && step.scopeId != talas::runtime::globalRuntimeScope
                && step.offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step.offlineBinding
                    == talas::core::ExecutionBinding::Kernel
                && step.preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset
                && step.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::Kernel;
        });
    require(perVoiceOscillatorCount == 2,
        "C160 mixed graph should retain two contextual per-voice both-Subset Oscillator kernels");

    talas::runtime::RealtimeRenderSession realtime{*compiled.graph};
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 4);
    std::array<double, 4> realtimeOutput{};
    require(realtime.ready()
            && realtime.gateTargetIndex("rootEnv").has_value()
            && !realtime.gateTargetIndex("env").has_value()
            && realtime.render(realtimeOutput)
            && offline.ok(),
        "C159 mixed graph should render in both engines and mint a typed target only for the exact root ADSR: "
            + std::string{realtime.error()});

    auto downgraded = *compiled.graph;
    auto& downgradedPlan = const_cast<talas::runtime::RuntimePlan&>(
        downgraded.plan());
    const auto downgradedRoot = std::ranges::find(
        downgradedPlan.steps, std::string_view{"rootEnv"},
        &talas::runtime::CompileStep::nodeId);
    require(downgradedRoot != downgradedPlan.steps.end(),
        "C159 mixed sealed-downgrade fixture should expose its exact root ADSR");
    downgradedRoot->offlineSupport =
        talas::core::ExecutionSupport::Subset;
    downgradedRoot->preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
    talas::runtime::RealtimeRenderSession rejectedRealtime{downgraded};
    const auto rejectedOffline = talas::runtime::renderOffline(
        downgraded, 4);
    require(!rejectedRealtime.ready() && !rejectedOffline.ok(),
        "C159 mixed authority should reject a sealed exact-root support downgrade");
}

void testMidiVoiceRejectsSharedIslandsAndLowersMultipleDisjointRegions()
{
    auto settings = midiVoiceCompileSettings();

    talas::project::ProjectDocument fanout;
    fanout.graph.nodes.push_back(node("out", "talas.audioOutput"));
    fanout.graph.nodes.push_back(node("midi", "talas.midiVoice", midiVoiceParameters({{"polyphony", "2"}})));
    fanout.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    fanout.graph.connections.push_back(connection("osc", "audioOut", "midi", "audioIn"));
    fanout.graph.connections.push_back(connection("midi", "leftOut", "out", "audioIn"));
    fanout.graph.connections.push_back(connection("osc", "audioOut", "out", "rightIn"));
    auto result = talas::runtime::compileProject(fanout, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "osc");

    auto nested = makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({{"polyphony", "2"}}));
    nested.graph.nodes.push_back(node("mapper", "talas.midiMapper", midiMapperParameters()));
    nested.graph.connections[0] = connection("osc", "audioOut", "mapper", "audioIn");
    nested.graph.connections.push_back(connection("mapper", "leftOut", "midi", "audioIn"));
    result = talas::runtime::compileProject(nested, makeRegistry(), settings);
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "mapper");

    talas::project::ProjectDocument multiple;
    multiple.graph.nodes.push_back(node("out", "talas.audioOutput"));
    multiple.graph.nodes.push_back(node("midi2", "talas.midiVoice", midiVoiceParameters()));
    multiple.graph.nodes.push_back(node("midi1", "talas.midiVoice", midiVoiceParameters()));
    multiple.graph.nodes.push_back(node("osc2", "talas.oscillator", {{"frequency", "2 hz"}}));
    multiple.graph.nodes.push_back(node("osc1", "talas.oscillator", {{"frequency", "1 hz"}}));
    multiple.graph.connections.push_back(connection("osc1", "audioOut", "midi1", "audioIn"));
    multiple.graph.connections.push_back(connection("osc2", "audioOut", "midi2", "audioIn"));
    multiple.graph.connections.push_back(connection("midi1", "leftOut", "out", "audioIn"));
    multiple.graph.connections.push_back(connection("midi2", "leftOut", "out", "rightIn"));
    result = talas::runtime::compileProject(multiple, makeRegistry(), settings);
    require(
        result.ok() && result.graph->plan().midiVoices.size() == 2,
        "disjoint reachable MIDI Voice regions should lower independently"
            + compileDiagnosticsSummary(result));
    const auto& regions = result.graph->plan().midiVoices;
    require(
        regions[0].voices.size() == 1
            && regions[1].voices.size() == 1
            && regions[0].voices.front().scopeId
                != regions[1].voices.front().scopeId
            && regions[0].voices.front().audioOutputBufferIndex
                != regions[1].voices.front().audioOutputBufferIndex,
        "multiple MIDI Voice regions should own disjoint scope and root-buffer state");
    require(
        result.graph->plan().steps[regions[0].ownerStepIndex].nodeId
                != result.graph->plan().steps[regions[1].ownerStepIndex].nodeId,
        "each MIDI Voice region should retain its own prepared owner step");
    require(
        regions[0].ownerNodeId == "midi1"
            && regions[1].ownerNodeId == "midi2",
        "MIDI Voice structural identities should remain deterministic across graph insertion order");
}

std::vector<talas::project::ProjectParameterExpression>
nestedKeyModeAnalogParameters(std::size_t polyphony)
{
    const auto threeVoices = polyphony == 3;
    return analogSequencerParameters({
        {"polyphony", std::to_string(polyphony)},
        textParameter("pitches", ""),
        textParameter("velocities", ""),
        textParameter("keyDowns", threeVoices ? "#(1 0 1)" : "#(1 0)"),
        textParameter("keyPitches", threeVoices ? "#(60 64 67)" : "#(60 64)"),
        textParameter("keyVelocities", threeVoices ? "#(0.8 0.6 0.4)" : "#(0.8 0.6)"),
        textParameter("keyTimbres", threeVoices ? "#(0.1 0.3 0.5)" : "#(0.1 0.3)"),
    });
}

talas::project::ProjectDocument makeNestedKeyOwnerCompileDocument(
    bool outerMidiVoice)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("outer", outerMidiVoice ? "talas.midiVoice" : "talas.analogSequencer",
            outerMidiVoice
                ? midiVoiceParameters({{"polyphony", "2"}})
                : nestedKeyModeAnalogParameters(2)),
        node("inner", "talas.analogSequencer", nestedKeyModeAnalogParameters(3)),
        node("tone", "talas.oscillator", {
            {"frequency", "1 hz"},
            {"amplitude", "@outerGain"}}),
        node("parentControl", "talas.constant", {{"value", "@outerGain"}}),
    };
    document.graph.connections = {
        connection("tone", "audioOut", "inner", "audioIn"),
        connection("parentControl", "value", "inner", "gate"),
        connection("inner", "leftOut", "outer", "audioIn"),
        connection("outer", "leftOut", "out", "audioIn"),
        connection("outer", "rightOut", "out", "rightIn"),
    };

    const auto addVoiceVariable = [&](std::string id, std::string ownerNodeId,
                                      double initialValue) {
        talas::project::ProjectVariableDeclaration declaration;
        declaration.variableId = std::move(id);
        declaration.displayName = declaration.variableId;
        declaration.initialValue = initialValue;
        declaration.snapshotEligible = false;
        declaration.owner.kind =
            talas::project::ProjectVariableOwnerKind::MidiVoice;
        declaration.owner.nodeId = std::move(ownerNodeId);
        document.variableLibrary.variables.push_back(std::move(declaration));
    };
    if (outerMidiVoice)
    {
        addVoiceVariable("outerGain", "outer", 0.75);
    }
    else
    {
        talas::project::ProjectVariableDeclaration projectGain;
        projectGain.variableId = "outerGain";
        projectGain.displayName = "Outer Gain";
        projectGain.initialValue = 0.75;
        projectGain.snapshotEligible = false;
        document.variableLibrary.variables.push_back(std::move(projectGain));
    }
    return document;
}

void testNestedKeyOwnerCompilePlansPreserveParentVoiceScopes()
{
    const auto settings = midiVoiceCompileSettings();
    for (const auto outerMidiVoice : {false, true})
    {
        const auto document = makeNestedKeyOwnerCompileDocument(outerMidiVoice);
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), settings);
        require(result.ok(),
            "2x3 nested Analog Input owners should compile;"
                + compileDiagnosticsSummary(result));
        const auto& plan = result.graph->plan();
        const auto parent = std::ranges::find_if(
            plan.midiVoices, [](const auto& candidate) {
                return candidate.ownerNodeId == "outer"
                    && candidate.voiceAncestors.empty();
            });
        require(parent != plan.midiVoices.end()
                && parent->voices.size() == 2
                && !parent->parentVoiceScopeId,
            "the root owner should retain exactly two unparented voice slots");
        require(std::ranges::count_if(
                    plan.midiVoices, [](const auto& candidate) {
                        return candidate.ownerNodeId == "inner";
                    }) == 2,
            "the inner owner should produce one plan for each parent voice");

        std::vector<talas::runtime::RuntimeScopeId> allVoiceScopes;
        const auto& variables = result.graph->preparedVariables();
        const auto outerVariableIndex = [&](const auto& ancestors,
                                            std::uint16_t ordinal) {
            const auto found = std::ranges::find_if(
                variables.cells, [&](const auto& cell) {
                    return cell.identity.variableId == "outerGain"
                        && cell.identity.ownerKind
                            == (outerMidiVoice
                                ? talas::project::ProjectVariableOwnerKind::MidiVoice
                                : talas::project::ProjectVariableOwnerKind::Project)
                        && (outerMidiVoice
                            ? cell.identity.ownerNodeId == "outer"
                                && cell.identity.voiceAncestors == ancestors
                                && cell.identity.voiceOrdinal
                                    == std::optional<std::uint16_t>{ordinal}
                            : cell.identity.ownerNodeId.empty()
                                && cell.identity.voiceAncestors.empty()
                                && !cell.identity.voiceOrdinal);
                });
            require(found != variables.cells.end(),
                "each nested graph should resolve its matching project or parent-voice scalar");
            return static_cast<std::size_t>(
                std::distance(variables.cells.begin(), found));
        };

        for (const auto& parentVoice : parent->voices)
        {
            allVoiceScopes.push_back(parentVoice.scopeId);
            const auto innerPlan = std::ranges::find_if(
                plan.midiVoices, [&](const auto& candidate) {
                    return candidate.ownerNodeId == "inner"
                        && candidate.parentVoiceScopeId
                            == parentVoice.scopeId;
                });
            require(innerPlan != plan.midiVoices.end()
                    && innerPlan->voices.size() == 3
                    && innerPlan->voiceAncestors.size() == 1
                    && innerPlan->voiceAncestors.front().ownerNodeId == "outer"
                    && innerPlan->voiceAncestors.front().voiceOrdinal
                        == parentVoice.voiceOrdinal,
                "each parent voice should own an exact three-voice child instance");

            const auto parentCell = outerVariableIndex(
                parent->voiceAncestors, parentVoice.voiceOrdinal);
            if (outerMidiVoice)
            {
                require(std::ranges::find(
                            parentVoice.variableCellIndices, parentCell)
                        != parentVoice.variableCellIndices.end(),
                    "the parent MIDI Voice plan should expose its declared scalar cell");
            }
            const auto parentControl = std::ranges::find_if(
                plan.steps, [&](const auto& step) {
                    return step.nodeId == "parentControl"
                        && step.scopeId == parentVoice.scopeId;
                });
            require(parentControl != plan.steps.end(),
                "the connected parent control should be cloned in the parent voice scope");
            const auto parentControlValue = std::ranges::find(
                parentControl->parameters, std::string{"value"},
                &talas::runtime::ParameterValue::id);
            require(parentControlValue != parentControl->parameters.end()
                    && parentControlValue->variableCellIndices.size() == 1
                    && parentControlValue->variableCellIndices.front() == parentCell,
                "the connected parent control should bind to its matching parent voice cell");

            const auto parentEnd = parentVoice.firstStepIndex + parentVoice.stepCount;
            require(parentEnd <= plan.steps.size(),
                "the parent voice interval should be valid after nested lowering");
            for (const auto& childVoice : innerPlan->voices)
            {
                allVoiceScopes.push_back(childVoice.scopeId);
                const auto toneStep = std::ranges::find_if(
                    plan.steps, [&](const auto& step) {
                        return step.nodeId == "tone"
                            && step.scopeId == childVoice.scopeId;
                    });
                require(toneStep != plan.steps.end(),
                    "every child voice should contain its own oscillator clone");
                const auto amplitude = std::ranges::find(
                    toneStep->parameters, std::string{"amplitude"},
                    &talas::runtime::ParameterValue::id);
                require(amplitude != toneStep->parameters.end()
                        && amplitude->variableCellIndices.size() == 1
                        && std::ranges::find(amplitude->variableCellIndices, parentCell)
                            != amplitude->variableCellIndices.end(),
                    "each child oscillator should bind the matching parent or project scalar");
                require(childVoice.firstStepIndex < innerPlan->ownerStepIndex
                        && childVoice.firstStepIndex >= parentVoice.firstStepIndex
                        && childVoice.firstStepIndex + childVoice.stepCount <= parentEnd,
                    "child descendants should be emitted inside their enclosing parent interval before the child mixer");
            }

            const auto& childMixer = plan.steps[innerPlan->ownerStepIndex];
            require(childMixer.nodeId == "inner"
                    && childMixer.scopeId == parentVoice.scopeId
                    && static_cast<std::size_t>(parentControl - plan.steps.begin())
                        < innerPlan->ownerStepIndex,
                "connected parent controls should precede the child mixer in the parent voice scope");
            const auto directParentSteps = std::ranges::count_if(
                plan.steps.begin()
                    + static_cast<std::ptrdiff_t>(parentVoice.firstStepIndex),
                plan.steps.begin() + static_cast<std::ptrdiff_t>(parentEnd),
                [&](const auto& step) {
                    return step.scopeId == parentVoice.scopeId;
                });
            require(directParentSteps == 2,
                "enclosing parent intervals should keep direct parent steps separate from child voice scopes");
        }

        std::ranges::sort(allVoiceScopes);
        require(std::ranges::adjacent_find(allVoiceScopes) == allVoiceScopes.end(),
            "every parent and child voice instance should own a distinct runtime scope");

        auto reordered = document;
        std::ranges::reverse(reordered.graph.nodes);
        std::ranges::reverse(reordered.graph.connections);
        const auto reorderedResult = talas::runtime::compileProject(
            reordered, makeRegistry(), settings);
        require(reorderedResult.ok(),
            "reordered nested Analog Input owners should compile;"
                + compileDiagnosticsSummary(reorderedResult));
        const auto& reorderedPlan = reorderedResult.graph->plan();
        const auto stepOrder = [](const auto& runtimePlan) {
            std::vector<std::pair<std::string, talas::runtime::RuntimeScopeId>> result;
            for (const auto& step : runtimePlan.steps)
                result.emplace_back(step.nodeId, step.scopeId);
            return result;
        };
        require(reorderedPlan.midiVoices == plan.midiVoices
                && reorderedPlan.buffers == plan.buffers
                && stepOrder(reorderedPlan) == stepOrder(plan),
            "nested voice scopes and clone order should be stable after input storage reordering");
    }
}

void testNestedMidiVoiceDeclaredCellsAndStepBudget()
{
    auto document = makeNestedKeyOwnerCompileDocument(true);
    document.expressionLanguage = talas::expr::ExpressionLanguageVersion::VoiceV1;
    auto& inner = *std::ranges::find(document.graph.nodes, "inner",
        &talas::project::ProjectNode::instanceId);
    inner.typeId = "talas.midiVoice";
    inner.parameters = midiVoiceParameters({{"polyphony", "3"}});
    std::erase_if(document.graph.connections, [](const auto& edge) {
        return edge.fromNode == "parentControl";
    });
    std::erase_if(document.graph.nodes, [](const auto& candidate) {
        return candidate.instanceId == "parentControl";
    });
    auto childGain = document.variableLibrary.variables.front();
    childGain.variableId = "childGain";
    childGain.owner.nodeId = "inner";
    childGain.initialValue = 0.1;
    document.variableLibrary.variables.push_back(childGain);
    auto& tone = *std::ranges::find(document.graph.nodes, "tone",
        &talas::project::ProjectNode::instanceId);
    std::ranges::find(tone.parameters, "amplitude",
        &talas::project::ProjectParameterExpression::parameterId)->expression =
            "@childGain + @outerGain + ?VoiceNumber / 100";
    const auto result = talas::runtime::compileProject(document, makeRegistry(), midiVoiceCompileSettings());
    require(result.ok(), "nested MIDI Voice declarations must compile;" + compileDiagnosticsSummary(result));
    std::vector<std::size_t> childCells;
    for (const auto& owner : result.graph->plan().midiVoices)
    {
        if (owner.ownerNodeId != "inner") continue;
        for (const auto& voice : owner.voices)
        {
            require(voice.variableCellCount == 1 && voice.variableCellIndices.size() == 1
                && voice.variableCellIndices.front() == voice.firstVariableCellIndex,
                "each child voice must retain its exact declared cell range and reset index");
            const auto& cell = result.graph->preparedVariables().cells[voice.firstVariableCellIndex];
            require(cell.identity.voiceAncestors == owner.voiceAncestors
                && cell.identity.voiceOrdinal == voice.voiceOrdinal,
                "same local child ordinals under different parents must have distinct declared identities");
            const auto oscillator = std::ranges::find_if(result.graph->plan().steps,
                [&](const auto& step) { return step.nodeId == "tone" && step.scopeId == voice.scopeId; });
            require(oscillator != result.graph->plan().steps.end(), "nested declared voice must own its tone");
            const auto amplitude = std::ranges::find(oscillator->parameters, "amplitude",
                &talas::runtime::ParameterValue::id);
            require(amplitude != oscillator->parameters.end()
                && std::abs(amplitude->value - (0.85 + (voice.voiceOrdinal + 1) / 100.0)) < 1e-14,
                "each parent instance must specialize the child's local VoiceNumber and scalar bindings");
            childCells.push_back(voice.firstVariableCellIndex);
        }
    }
    std::ranges::sort(childCells);
    require(childCells.size() == 6 && std::ranges::adjacent_find(childCells) == childCells.end(),
        "all six nested declared cells must remain separate");

    auto oversized = makeNestedKeyOwnerCompileDocument(false);
    auto& owner = *std::ranges::find(oversized.graph.nodes, "inner",
        &talas::project::ProjectNode::instanceId);
    std::ranges::find(owner.parameters, "polyphony",
        &talas::project::ProjectParameterExpression::parameterId)->expression = "128";
    std::string previous = "tone";
    for (int index = 0; index < 32; ++index)
    {
        const auto id = "gain" + std::to_string(index);
        oversized.graph.nodes.push_back(node(id, "talas.vca", {{"gain", "0.5"}}));
        oversized.graph.connections.push_back(connection(previous, "audioOut", id, "audioIn"));
        previous = id;
    }
    std::ranges::find_if(oversized.graph.connections, [](const auto& edge) {
        return edge.toNode == "inner" && edge.toPort == "audioIn";
    })->fromNode = previous;
    const auto rejected = talas::runtime::compileProject(oversized, makeRegistry(), midiVoiceCompileSettings());
    require(!rejected.ok() && !rejected.graph
        && std::ranges::any_of(rejected.diagnostics, [](const auto& diagnostic) {
            return diagnostic.message.find("processing step capacity") != std::string::npos;
        }), "nested processor expansion must reject over-budget multiplication before publication");
}

talas::project::ProjectDocument makeNestedSoundDefaultVoiceDocument()
{
    talas::project::ProjectSoundDefinition toneSound;
    toneSound.definitionId = "toneSound";
    toneSound.displayName = "Tone Sound";
    toneSound.graph.nodes.push_back(node("tone", "talas.oscillator", {
        {"frequency", "1 hz"},
        {"amplitude", "@toneGain + @outerGain"},
    }));
    toneSound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "tone", "audioOut"});

    talas::project::ProjectSoundDefinition innerSound;
    innerSound.definitionId = "innerSound";
    innerSound.displayName = "Inner Sound";
    talas::project::ProjectNode toneInstance;
    toneInstance.instanceId = "toneInstance";
    toneInstance.soundDefinitionId = "toneSound";
    innerSound.graph.nodes = {
        node("inner", "talas.analogSequencer", nestedKeyModeAnalogParameters(3)),
        std::move(toneInstance),
    };
    innerSound.graph.connections.push_back(
        connection("toneInstance", "audioOut", "inner", "audioIn"));
    innerSound.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "inner", "leftOut"});

    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        std::move(toneSound),
        std::move(innerSound),
        {.definitionId = "choiceA", .displayName = "Choice A"},
        {.definitionId = "choiceB", .displayName = "Choice B"},
    };
    talas::project::ProjectNode insideInstance;
    insideInstance.instanceId = "insideInstance";
    insideInstance.soundDefinitionId = "innerSound";
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("outer", "talas.midiVoice", midiVoiceParameters({{"polyphony", "2"}})),
        std::move(insideInstance),
    };
    document.graph.connections = {
        connection("insideInstance", "audioOut", "outer", "audioIn"),
        connection("outer", "leftOut", "out", "audioIn"),
        connection("outer", "rightOut", "out", "rightIn"),
    };

    talas::project::ProjectVariableDeclaration projectOuterGain;
    projectOuterGain.variableId = "outerGain";
    projectOuterGain.displayName = "Project Outer Gain";
    projectOuterGain.initialValue = 0.5;
    projectOuterGain.snapshotEligible = false;

    talas::project::ProjectVariableDeclaration outerGain;
    outerGain.variableId = "outerGain";
    outerGain.displayName = "Outer Gain";
    outerGain.initialValue = 0.75;
    outerGain.snapshotEligible = false;
    outerGain.owner.kind = talas::project::ProjectVariableOwnerKind::MidiVoice;
    outerGain.owner.nodeId = "outer";
    talas::project::ProjectVariableDeclaration toneGain;
    toneGain.variableId = "toneGain";
    toneGain.displayName = "Tone Gain";
    toneGain.initialValue = 0.25;
    toneGain.snapshotEligible = false;
    toneGain.owner.kind = talas::project::ProjectVariableOwnerKind::SoundDefinition;
    toneGain.owner.definitionId = "toneSound";
    document.variableLibrary.variables = {
        projectOuterGain, outerGain, toneGain};

    talas::project::ProjectSoundVariableDeclaration toneChoice;
    toneChoice.variableId = "toneChoice";
    toneChoice.displayName = "Tone Choice";
    toneChoice.candidateDefinitionIds = {"choiceA", "choiceB"};
    toneChoice.snapshotEligible = false;
    toneChoice.owner.kind = talas::project::ProjectVariableOwnerKind::SoundDefinition;
    toneChoice.owner.definitionId = "toneSound";
    document.soundVariableLibrary.variables.push_back(std::move(toneChoice));
    return document;
}

void testNestedVoicePlansBindProjectedDescendantSoundDefaults()
{
    const auto result = talas::runtime::compileProject(
        makeNestedSoundDefaultVoiceDocument(), makeRegistry(),
        midiVoiceCompileSettings());
    require(result.ok(),
        "nested voice graph with descendant Sound defaults should compile;"
            + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    const auto outer = std::ranges::find_if(
        plan.midiVoices, [](const auto& candidate) {
            return candidate.ownerNodeId == "outer"
                && candidate.voiceAncestors.empty();
        });
    require(outer != plan.midiVoices.end() && outer->voices.size() == 2,
        "the outer MIDI Voice should prepare two parent scopes");

    const auto& variables = result.graph->preparedVariables();
    const auto& soundVariables = result.graph->preparedSoundVariables();
    std::vector<std::size_t> scalarCopies;
    std::vector<std::size_t> soundCopies;
    for (const auto& parentVoice : outer->voices)
    {
        const auto inner = std::ranges::find_if(
            plan.midiVoices, [&](const auto& candidate) {
                return candidate.ownerNodeId == "inner"
                    && candidate.parentVoiceScopeId == parentVoice.scopeId;
            });
        require(inner != plan.midiVoices.end() && inner->voices.size() == 3,
            "each parent scope should contain one three-voice Analog Sequencer instance");
        for (const auto& childVoice : inner->voices)
        {
            auto ancestors = inner->voiceAncestors;
            ancestors.push_back({inner->ownerNodeId, inner->ownerDefinitionId,
                inner->ownerInstancePath, childVoice.voiceOrdinal});
            const auto scalar = std::ranges::find_if(
                variables.cells, [&](const auto& cell) {
                    return cell.identity.variableId == "toneGain"
                        && cell.identity.ownerKind
                            == talas::project::ProjectVariableOwnerKind::SoundDefinition
                        && cell.identity.definitionId == "toneSound"
                        && cell.identity.voiceAncestors == ancestors;
                });
            const auto sound = std::ranges::find_if(
                soundVariables.cells, [&](const auto& cell) {
                    return cell.identity.variableId == "toneChoice"
                        && cell.identity.ownerKind
                            == talas::project::ProjectVariableOwnerKind::SoundDefinition
                        && cell.identity.definitionId == "toneSound"
                        && cell.identity.voiceAncestors == ancestors;
                });
            require(scalar != variables.cells.end()
                    && sound != soundVariables.cells.end(),
                "every nested voice should receive exact projected scalar and Sound defaults");
            const auto scalarIndex = static_cast<std::size_t>(
                std::distance(variables.cells.begin(), scalar));
            const auto soundIndex = static_cast<std::size_t>(
                std::distance(soundVariables.cells.begin(), sound));
            require(std::ranges::find(childVoice.variableCellIndices, scalarIndex)
                        != childVoice.variableCellIndices.end()
                    && std::ranges::find(childVoice.soundVariableCellIndices, soundIndex)
                        != childVoice.soundVariableCellIndices.end(),
                "nested voice plans should list their noncontiguous descendant Sound default cells");
            scalarCopies.push_back(scalarIndex);
            soundCopies.push_back(soundIndex);

            const auto oscillator = std::ranges::find_if(
                plan.steps, [&](const auto& step) {
                    return step.typeId == "talas.oscillator"
                        && step.scopeId == childVoice.scopeId;
                });
            require(oscillator != plan.steps.end(),
                "each nested voice should lower its descendant Sound oscillator");
            const auto amplitude = std::ranges::find(
                oscillator->parameters, std::string{"amplitude"},
                &talas::runtime::ParameterValue::id);
            const auto parentCell = std::ranges::find_if(
                variables.cells, [&](const auto& cell) {
                    return cell.identity.variableId == "outerGain"
                        && cell.identity.ownerKind
                            == talas::project::ProjectVariableOwnerKind::MidiVoice
                        && cell.identity.ownerNodeId == "outer"
                        && cell.identity.voiceOrdinal
                            == std::optional<std::uint16_t>{
                                parentVoice.voiceOrdinal};
                });
            require(amplitude != oscillator->parameters.end()
                    && amplitude->variableCellIndices.size() == 2
                    && parentCell != variables.cells.end()
                    && std::ranges::find(amplitude->variableCellIndices, scalarIndex)
                        != amplitude->variableCellIndices.end()
                    && std::ranges::find(
                        amplitude->variableCellIndices,
                        static_cast<std::size_t>(
                            std::distance(variables.cells.begin(), parentCell)))
                        != amplitude->variableCellIndices.end(),
                "descendant Sound expressions should bind their projected default and inherit the matching parent scalar");
        }
    }
    std::ranges::sort(scalarCopies);
    std::ranges::sort(soundCopies);
    require(scalarCopies.size() == 6 && soundCopies.size() == 6
            && std::ranges::adjacent_find(scalarCopies) == scalarCopies.end()
            && std::ranges::adjacent_find(soundCopies) == soundCopies.end(),
        "2x3 parent and child voices should own six distinct projected Sound defaults");
}

void testMidiVoiceVariableCapacityFailsBeforePublication()
{
    auto document = makePreparedMidiVoiceOutputDocument(
        midiVoiceParameters({{"polyphony", "128"}}));
    for (std::size_t index = 0; index < 17; ++index)
    {
        talas::project::ProjectVariableDeclaration declaration;
        declaration.variableId = "voiceValue" + std::to_string(index);
        declaration.displayName = declaration.variableId;
        declaration.initialValue = static_cast<double>(index);
        declaration.snapshotEligible = false;
        declaration.owner.kind =
            talas::project::ProjectVariableOwnerKind::MidiVoice;
        declaration.owner.nodeId = "midi";
        document.variableLibrary.variables.push_back(
            std::move(declaration));
    }

    const auto result = talas::runtime::compileProject(
        document,
        makeRegistry(),
        midiVoiceCompileSettings());
    require(
        !result.ok()
            && compileDiagnosticsSummary(result).find(
                   "prepared capacity")
                != std::string::npos,
        "voice-cell expansion beyond the fixed prepared capacity should fail before graph publication");
}

void testMidiVoiceConfigurationLimitsAreEnforced()
{
    auto settings = midiVoiceCompileSettings();
    auto compileWith = [&](std::vector<talas::project::ProjectParameterExpression> overrides) {
        return talas::runtime::compileProject(
            makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters(std::move(overrides))),
            makeRegistry(),
            settings);
    };

    auto result = compileWith({{"channel", "0"}});
    require(result.ok(), "channel zero should resolve through render settings;" + compileDiagnosticsSummary(result));
    require(result.graph->plan().midiVoices.front().configuration.channel == settings.defaultMidiChannel,
            "channel zero should compile to the configured default channel");

    result = compileWith({{"channel", "17"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "channel");
    result = compileWith({{"channel", "1.5"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "channel");
    result = compileWith({{"polyphony", "129"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "polyphony");
    result = compileWith({{"polyphony", "1.5"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "polyphony");
    result = compileWith({{"lowPitch", "-1 nn"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "lowPitch");
    result = compileWith({{"highPitch", "128 nn"}});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "highPitch");

    settings.defaultMidiChannel = 0;
    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters()),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);
}

void requireNoAudioTerminalCompilePlan(
    const talas::runtime::CompileResult& result,
    std::string_view expectedTypeId)
{
    require(result.ok(), std::string(expectedTypeId) + " terminal-only graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.steps.size() == 1, std::string(expectedTypeId) + " terminal-only graph should contain one step");
    require(plan.buffers.empty(), std::string(expectedTypeId) + " terminal should not allocate audio buffers");
    require(plan.outputBufferIndices.empty(), std::string(expectedTypeId) + " terminal should not expose output buffer indices");

    const auto& terminal = requireStep(plan, "terminal");
    require(terminal.typeId == expectedTypeId, std::string(expectedTypeId) + " step should retain node type");
    require(terminal.inputs.empty(), std::string(expectedTypeId) + " terminal should not require input bindings");
}

void testNoAudioTerminalGraphsCompileAsNoopOutputs()
{
    auto result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiFileEcho", midiFileEchoParameters()),
        makeRegistry(),
        {});
    requireNoAudioTerminalCompilePlan(result, "talas.midiFileEcho");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "lowChannel").value, 1.0, 1.0e-12, "MIDIFileEcho lowChannel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "highChannel").value, 16.0, 1.0e-12, "MIDIFileEcho highChannel should compile");
    requireResourceParameter(requireStep(result.graph->plan(), "terminal"), "fileName", "");
    const auto& midiFileTerminal =
        requireStep(result.graph->plan(), "terminal");
    require(
        midiFileTerminal.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && midiFileTerminal.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiFileTerminal.supportsReset
            && midiFileTerminal.resolvedLatencyFrames == 0
            && midiFileTerminal.cumulativeInputLatencyFrames == 0
            && midiFileTerminal.cumulativeOutputLatencyFrames == 0,
        "MIDIFileEcho should compile exact C42 prepared terminal authority");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputController", midiOutputControllerParameters()),
        makeRegistry(),
        {});
    requireNoAudioTerminalCompilePlan(result, "talas.midiOutputController");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "channel").value, 1.0, 1.0e-12, "MIDIOutputController channel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "controllerNumber").value, 64.0, 1.0e-12, "MIDIOutputController controllerNumber should compile");
    requireTextParameter(requireStep(result.graph->plan(), "terminal"), "value", "");
    const auto& controllerTerminal =
        requireStep(result.graph->plan(), "terminal");
    require(
        controllerTerminal.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && controllerTerminal.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && controllerTerminal.supportsReset
            && controllerTerminal.resolvedLatencyFrames == 0
            && controllerTerminal.cumulativeInputLatencyFrames == 0
            && controllerTerminal.cumulativeOutputLatencyFrames == 0,
        "MIDIOutputController should compile exact C40 prepared terminal authority");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputEvent", midiOutputEventParameters()),
        makeRegistry(),
        {});
    requireNoAudioTerminalCompilePlan(result, "talas.midiOutputEvent");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "frequency").value, 440.0, 1.0e-12, "MIDIOutputEvent frequency should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "amplitude").value, 0.5, 1.0e-12, "MIDIOutputEvent amplitude should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "channel").value, 1.0, 1.0e-12, "MIDIOutputEvent channel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "terminal"), "gate").value, 0.0, 1.0e-12, "MIDIOutputEvent gate should compile");
    const auto& noteTerminal =
        requireStep(result.graph->plan(), "terminal");
    require(
        noteTerminal.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && noteTerminal.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && noteTerminal.supportsReset
            && noteTerminal.resolvedLatencyFrames == 0
            && noteTerminal.cumulativeInputLatencyFrames == 0
            && noteTerminal.cumulativeOutputLatencyFrames == 0,
        "MIDIOutputEvent should compile exact C41 prepared terminal authority");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputEventInBytes", midiOutputEventInBytesParameters()),
        makeRegistry(),
        {});
    requireNoAudioTerminalCompilePlan(result, "talas.midiOutputEventInBytes");
    requireTextParameter(requireStep(result.graph->plan(), "terminal"), "bytes", "");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.soundToGlobalController", soundToGlobalControllerParameters()),
        makeRegistry(),
        {});
    requireNoAudioTerminalCompilePlan(result, "talas.soundToGlobalController");
    requireTextParameter(requireStep(result.graph->plan(), "terminal"), "generatedEvent", "");
    requireTextParameter(requireStep(result.graph->plan(), "terminal"), "value", "");

    auto dynamicSettings = talas::runtime::RenderSettings{};
    dynamicSettings.eventValues = {
        {"ControllerValue", 0.625,
         talas::expr::ValueDomain::Scalar},
    };
    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument(
            "talas.soundToGlobalController",
            soundToGlobalControllerParameters({
                textParameter("generatedEvent", "!Generated"),
                textParameter("value", "{!ControllerValue}"),
            })),
        makeRegistry(),
        dynamicSettings);
    requireNoAudioTerminalCompilePlan(
        result, "talas.soundToGlobalController");
    const auto& fallback = requireParameter(
        requireStep(result.graph->plan(), "terminal"), "value");
    require(
        fallback.text == "{!ControllerValue}"
            && fallback.expression.has_value()
            && fallback.expression->eventReferences()
                == std::vector<std::string>{"ControllerValue"}
            && fallback.domain
                == talas::expr::ValueDomain::Scalar
            && fallback.value == 0.625
            && fallback.variableScopeId
                == talas::runtime::globalRuntimeScope
            && fallback.variableCellIndices.empty(),
        "C48 dynamic fallback should compile exact text-backed Scalar Event Value authority");
}

void testStateSinkTerminalGraphsCompileAsNoopOutputs()
{
    auto result = talas::runtime::compileProject(
        makeFeedbackLoopInputTerminalOutputDocument(),
        makeRegistry(),
        {});
    require(result.ok(), "FeedbackLoopInput terminal sink graph should compile;" + compileDiagnosticsSummary(result));
    const auto& feedbackPlan = result.graph->plan();
    require(feedbackPlan.steps.size() == 2, "FeedbackLoopInput terminal graph should contain oscillator and terminal steps");
    require(feedbackPlan.outputBufferIndices.empty(), "FeedbackLoopInput terminal should not expose output buffer indices");
    require(hasBuffer(feedbackPlan, "osc", "audioOut"), "FeedbackLoopInput terminal graph should allocate upstream oscillator buffer");

    const auto& feedback = requireStep(feedbackPlan, "terminal");
    require(feedback.typeId == "talas.feedbackLoopInput", "FeedbackLoopInput terminal step should retain node type");
    require(requireInputBinding(feedback, "audioIn").sourceNodeId == "osc", "FeedbackLoopInput audioIn should bind oscillator");
    requireTextParameter(feedback, "connection", "");

}

void testQuadOscillatorGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeQuadOscillatorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "quad oscillator graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "quad oscillator output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "quad", "quad oscillator output buffer should point to quad");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "quad oscillator output buffer should point to audioOut");

    const auto& quad = requireStep(plan, "quad");
    requireNear(requireParameter(quad, "frequency").value, 1.0, 1.0e-12, "quad oscillator frequency should compile");
    require(requireInputBinding(quad, "envelope").sourceNodeId == "input", "quad oscillator envelope should bind audio input");
    require(
        quad.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && quad.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && quad.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && quad.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && quad.supportsReset
            && quad.resolvedLatencyFrames == 0
            && quad.cumulativeOutputLatencyFrames == 0
            && !quad.kernelKind.has_value()
            && !quad.preparedIsolatedComponent,
        "quad oscillator compile plan should preserve exact C22 prepared authority");
}

void testThresholdCompilePlanPublishesStateAndResetAuthority()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("vca", "talas.vca"),
        node("carrier", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1"},
        }),
        node("source", "talas.constant", {{"value", "0.8"}}),
        node("threshold", "talas.threshold", {
            {"threshold", "0.5"}, {"hysteresis", "0.4"},
        }),
    };
    document.graph.connections = {
        {"source", "value", "threshold", "value"},
        {"carrier", "audioOut", "vca", "audioIn"},
        {"threshold", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {4.0, 3});
    require(result.ok(), "C66 Threshold compile-plan fixture should compile");
    const auto& threshold = requireStep(result.graph->plan(), "threshold");
    require(
        threshold.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && threshold.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && threshold.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && threshold.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && threshold.supportsReset
            && threshold.resolvedLatencyFrames == 0
            && threshold.cumulativeOutputLatencyFrames == 0
            && !threshold.kernelKind.has_value()
            && !threshold.preparedIsolatedComponent,
        "C181 Threshold compile plan should preserve exact Supported state/reset authority");
}

void testSumOfSinesGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSumOfSinesOutputDocument(), makeRegistry(), {});

    require(result.ok(), "sum of sines graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "sum of sines output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "sum", "sum of sines output buffer should point to sum");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "sum of sines output buffer should point to audioOut");

    const auto& sum = requireStep(plan, "sum");
    require(sum.typeId == "talas.sumOfSines", "sum of sines step should retain node type");
    requireNear(requireParameter(sum, "onDuration").value, 1.0, 1.0e-12, "sum of sines onDuration should compile");
    requireNear(requireParameter(sum, "frequency0").value, 100.0, 1.0e-9, "sum of sines frequency0 should compile");
    requireNear(requireParameter(sum, "frequency1").value, 100.0, 1.0e-9, "sum of sines frequency1 should compile");
    requireResourceParameter(sum, "analysis0", "Tables/Partials");
    requireNear(requireParameter(sum, "analysisFrequency0").value, 1.0, 1.0e-12, "sum of sines analysisFrequency0 should compile");
    requireResourceParameter(sum, "analysis1", "");
    requireNear(requireParameter(sum, "analysisFrequency1").value, 1.0, 1.0e-12, "sum of sines analysisFrequency1 should compile");
    requireNear(requireParameter(sum, "dbMorph").value, 0.0, 1.0e-12, "sum of sines dbMorph should compile");
    requireNear(requireParameter(sum, "pchMorph").value, 0.0, 1.0e-12, "sum of sines pchMorph should compile");
    requireNear(requireParameter(sum, "nbrPartials").value, 3.0, 1.0e-12, "sum of sines nbrPartials should compile");
    requireNear(requireParameter(sum, "bankSize").value, 3.0, 1.0e-12, "sum of sines bankSize should compile");
    requireNear(requireParameter(sum, "timeIndex").value, 0.0, 1.0e-12, "sum of sines timeIndex should compile");
    requireNear(requireParameter(sum, "gate").value, 1.0, 1.0e-12, "sum of sines gate should compile");
    requireBooleanParameter(sum, "loop", false);
    requireNear(requireParameter(sum, "loopStart").value, 0.0, 1.0e-12, "sum of sines loopStart should compile");
    requireNear(requireParameter(sum, "loopEnd").value, 0.0, 1.0e-12, "sum of sines loopEnd should compile");
    requireBooleanParameter(sum, "ctrlTime", false);
    requireNear(requireParameter(sum, "envelope").value, 0.5, 1.0e-12, "sum of sines envelope should compile");
}

void testC61SumOfSinesCompilesWithDefaultAnalysisFrequencies()
{
    auto parameters = sumOfSinesParameters({
        {"frequency0", "0 hz"},
        {"frequency1", "0 hz"},
        {"analysisFrequency0", "125 hz"},
        {"analysisFrequency1", "250 hz"},
    });
    std::erase_if(
        parameters,
        [](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == "frequency0"
                || parameter.parameterId == "frequency1";
        });

    const auto result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(std::move(parameters)),
        makeRegistry(),
        {});

    require(
        result.ok(),
        "C61 SumOfSines should compile omitted frequency defaults;"
            + compileDiagnosticsSummary(result));
    const auto& sum = requireStep(result.graph->plan(), "sum");
    requireNear(
        requireParameter(sum, "frequency0").value,
        0.0,
        0.0,
        "C61 omitted frequency0 should compile as the static zero sentinel");
    requireNear(
        requireParameter(sum, "analysisFrequency0").value,
        125.0,
        0.0,
        "C61 analysisFrequency0 should retain project-authored base pitch");
    requireNear(
        requireParameter(sum, "frequency1").value,
        0.0,
        0.0,
        "C61 omitted frequency1 should compile as the static zero sentinel");
    requireNear(
        requireParameter(sum, "analysisFrequency1").value,
        250.0,
        0.0,
        "C61 analysisFrequency1 should retain project-authored base pitch");
    require(
        sum.parameters.size() == 20
            && sum.parameters[0].id == "onDuration"
            && sum.parameters[1].id == "frequency0"
            && sum.parameters[2].id == "frequency1"
            && sum.parameters[3].id == "analysis0"
            && sum.parameters[4].id == "analysisFrequency0"
            && sum.parameters[5].id == "analysisFrames0"
            && sum.parameters[6].id == "analysis1"
            && sum.parameters[7].id == "analysisFrequency1"
            && sum.parameters[8].id == "analysisFrames1"
            && sum.parameters[9].id == "dbMorph"
            && sum.parameters[10].id == "pchMorph"
            && sum.parameters[11].id == "nbrPartials"
            && sum.parameters[12].id == "bankSize"
            && sum.parameters[13].id == "timeIndex"
            && sum.parameters[14].id == "gate"
            && sum.parameters[15].id == "loop"
            && sum.parameters[16].id == "loopStart"
            && sum.parameters[17].id == "loopEnd"
            && sum.parameters[18].id == "ctrlTime"
            && sum.parameters[19].id == "envelope",
        "C62 SumOfSines should compile the exact frame-metadata parameter authority");
}

void testC62SumOfSinesCompilesWithIndependentAnalysisFrames()
{
    auto parameters = sumOfSinesParameters({
        {"analysisFrames0", "3"},
        {"analysisFrames1", "5"},
        {"timeIndex", "-0.25"},
        booleanParameter("ctrlTime", true),
    });
    const auto result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(std::move(parameters)),
        makeRegistry(),
        {});

    require(result.ok(), "C62 SumOfSines controlled-time frame metadata should compile;" + compileDiagnosticsSummary(result));
    const auto& sum = requireStep(result.graph->plan(), "sum");
    require(requireParameter(sum, "analysisFrames0").value == 3.0,
        "C62 should retain Analysis0 frame authority");
    require(requireParameter(sum, "analysisFrames1").value == 5.0,
        "C62 should retain independent Analysis1 frame authority");
    require(requireParameter(sum, "timeIndex").value == -0.25,
        "C62 should retain the static normalized selector");
}

void testC63SumOfSinesCompilesAutomaticGateLoopTraversal()
{
    auto result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            {"analysisFrames0", "3"},
            {"analysisFrames1", "5"},
            booleanParameter("ctrlTime", false),
        })),
        makeRegistry(),
        {});
    require(result.ok(),
        "C63 automatic one-shot should admit bounded multi-frame analyses;"
            + compileDiagnosticsSummary(result));

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            {"analysisFrames0", "3"},
            {"analysisFrames1", "5"},
            booleanParameter("loop", true),
            {"loopStart", "0.25"},
            {"loopEnd", "0.75"},
            booleanParameter("ctrlTime", false),
        })),
        makeRegistry(),
        {});
    require(result.ok(),
        "C63 automatic loop should compile exact normalized static points;"
            + compileDiagnosticsSummary(result));

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            {"loopStart", "-2"},
            {"loopEnd", "3"},
            booleanParameter("ctrlTime", false),
        })),
        makeRegistry(),
        {});
    require(result.ok(),
        "C63 inactive loop points should remain ignored for compatibility;"
            + compileDiagnosticsSummary(result));
}

void testSumOfSinesMorphParametersCompile()
{
    const auto result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            resourceParameter("analysis1", "Tables/AltPartials"),
            {"dbMorph", "0.25"},
            {"pchMorph", "0.75"},
        })),
        makeRegistry(),
        {});

    require(result.ok(), "sum of sines morph parameters should compile");
    const auto& sum = requireStep(result.graph->plan(), "sum");
    requireResourceParameter(sum, "analysis1", "Tables/AltPartials");
    requireNear(requireParameter(sum, "dbMorph").value, 0.25, 1.0e-12, "sum of sines dbMorph should compile");
    requireNear(requireParameter(sum, "pchMorph").value, 0.75, 1.0e-12, "sum of sines pchMorph should compile");
}

void testGaOscillatorsGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeGaOscillatorsOutputDocument(), makeRegistry(), {});

    require(result.ok(), "ga oscillators graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "ga oscillators output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "ga", "ga oscillators output buffer should point to ga");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "ga oscillators output buffer should point to audioOut");

    const auto& ga = requireStep(plan, "ga");
    require(ga.typeId == "talas.gaOscillators", "ga oscillators step should retain node type");
    require(ga.inputs.empty(), "ga oscillators should not require inputs");
    requireNear(requireParameter(ga, "timeIndex").value, 0.0, 1.0e-12, "ga oscillators timeIndex should compile");
    requireResourceParameter(ga, "analysis0", "Tables/GA");
    requireResourceParameter(ga, "analysis1", "");
    requireNear(requireParameter(ga, "envelope").value, 0.25, 1.0e-12, "ga oscillators envelope should compile");
    requireNear(requireParameter(ga, "frequency").value, 1.0, 1.0e-12, "ga oscillators frequency should compile");
    requireNear(requireParameter(ga, "morph").value, 0.0, 1.0e-12, "ga oscillators morph should compile");
    require(
        ga.parameters.size() == 6
            && ga.parameters[0].id == "timeIndex"
            && ga.parameters[1].id == "analysis0"
            && ga.parameters[2].id == "analysis1"
            && ga.parameters[3].id == "envelope"
            && ga.parameters[4].id == "frequency"
            && ga.parameters[5].id == "morph"
            && ga.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && ga.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ga.supportsReset
            && !ga.kernelKind.has_value()
            && ga.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && ga.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && ga.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !ga.preparedIsolatedComponent,
        "ga oscillators compile plan should preserve exact C32 prepared authority");
}

void testGaOscillatorsMorphParametersCompile()
{
    const auto result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({
            resourceParameter("analysis1", "Tables/AltGA"),
            {"morph", "0.5"},
        })),
        makeRegistry(),
        {});

    require(result.ok(), "ga oscillators morph parameters should compile");
    const auto& ga = requireStep(result.graph->plan(), "ga");
    requireResourceParameter(ga, "analysis1", "Tables/AltGA");
    requireNear(requireParameter(ga, "morph").value, 0.5, 1.0e-12, "ga oscillators morph should compile");
}

void testOscillatorBankGraphCompilesWithSpectrumInputAndAudioOutput()
{
    const auto result = talas::runtime::compileProject(makeOscillatorBankOutputDocument(), makeRegistry(), {});

    require(result.ok(), "oscillator bank graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "oscillator bank output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "bank", "oscillator bank output should point to bank node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "oscillator bank output should point to audioOut");

    const auto& bank = requireStep(plan, "bank");
    require(bank.typeId == "talas.oscillatorBank", "oscillator bank step should retain node type");
    require(requireInputBinding(bank, "spectrum").sourceNodeId == "spectrum", "oscillator bank should bind synthetic spectrum");
    requireNear(requireParameter(bank, "nbrOscillators").value, 2.0, 1.0e-12, "oscillator bank oscillator count should compile");
    requireNear(requireParameter(bank, "bankSize").value, 2.0, 1.0e-12, "oscillator bank size should compile");
    requireResourceParameter(bank, "wavetable", "");
    require(
        bank.parameters.size() == 4
            && bank.parameters[0].id == "nbrOscillators"
            && bank.parameters[1].id == "bankSize"
            && bank.parameters[2].id == "wavetable"
            && bank.parameters[3].id == "pairFrequencies"
            && bank.parameters[3].choice == std::optional<std::string>{"halfSampleRate"}
            && bank.parameters[0].expression.has_value()
            && bank.parameters[1].expression.has_value()
            && !bank.parameters[2].expression.has_value()
            && bank.parameters[2].resource.has_value(),
        "oscillator bank should preserve exact C24 parameter authority");
    require(
        bank.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && bank.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && bank.supportsReset
            && !bank.kernelKind.has_value()
            && bank.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && bank.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{
                    std::size_t{0}}
            && bank.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && bank.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "oscillator bank compile plan should preserve exact C24 prepared authority");
}

void testSosOscillatorsGraphCompilesWithSpectrumCascadeAndAudioOutput()
{
    const auto result = talas::runtime::compileProject(makeSosOscillatorsOutputDocument(), makeRegistry(), {});

    require(result.ok(), "sos oscillators graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "sos oscillators output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "sos", "sos oscillators output should point to sos node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "sos oscillators output should point to audioOut");

    const auto& sos = requireStep(plan, "sos");
    require(sos.typeId == "talas.sosOscillators", "sos oscillators step should retain node type");
    require(requireInputBinding(sos, "spectrum").sourceNodeId == "spectrum", "sos oscillators should bind synthetic spectrum");
    require(requireInputBinding(sos, "cascadeInput").sourceNodeId == "cascade", "sos oscillators should bind cascade input");
    requireNear(requireParameter(sos, "nbrOscillators").value, 2.0, 1.0e-12, "sos oscillators oscillator count should compile");
    requireNear(requireParameter(sos, "firstOscillator").value, 1.0, 1.0e-12, "sos oscillators first oscillator should compile");
    requireResourceParameter(sos, "wavetable", "");
    require(
        sos.inputs.size() == 2
            && sos.inputs[0].inputPortId == "spectrum"
            && sos.inputs[1].inputPortId == "cascadeInput",
        "C102 SOS Oscillators should preserve exact ordered input authority");
    require(
        sos.parameters.size() == 3
            && sos.parameters[0].id == "nbrOscillators"
            && sos.parameters[1].id == "firstOscillator"
            && sos.parameters[2].id == "wavetable",
        "C102 SOS Oscillators should preserve exact ordered parameter authority");
    require(
        sos.offlineSupport == talas::core::ExecutionSupport::Supported
            && sos.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sos.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && sos.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sos.supportsReset
            && !sos.kernelKind.has_value()
            && !sos.preparedIsolatedComponent
            && sos.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && sos.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{
                    std::size_t{0}, std::size_t{0}}
            && sos.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && sos.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !sos.preparedSpectralFrameSize.has_value()
            && !sos.preparedSpectralHopSize.has_value(),
        "C167 SOS Oscillators compile plan should preserve exact promoted reset, binding, and zero-latency authority");
}

void testSosOscillatorsRejectsNonintegralSelectionAndDynamicSpectrumAuthority()
{
    for (const auto parameterId : {std::string_view{"nbrOscillators"},
                                   std::string_view{"firstOscillator"}})
    {
        auto parameters = sosOscillatorsParameters();
        const auto parameter = std::ranges::find(
            parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(parameter != parameters.end(),
            "C102 integral-policy fixture should expose "
                + std::string{parameterId});
        parameter->expression = "1.0000000005";
        const auto result = talas::runtime::compileProject(
            makeSosOscillatorsOutputDocument(std::move(parameters)),
            makeRegistry(),
            {});
        requireCompileFailure(
            result,
            DiagnosticCode::ParameterEvaluationError,
            "sos",
            parameterId);
    }

    auto diskDocument = makeSosOscillatorsOutputDocument();
    const auto diskSource = std::ranges::find(
        diskDocument.graph.nodes,
        std::string_view{"spectrum"},
        &talas::project::ProjectNode::instanceId);
    require(diskSource != diskDocument.graph.nodes.end(),
        "C102 dynamic-authority fixture should expose its Spectrum source");
    *diskSource = node(
        "spectrum",
        "talas.spectrumOnDisk",
        spectrumOnDiskParameters({
            {"frequency", "1 hz"},
            {"level", "1"},
            resourceParameter("fileName", "Analysis/C102Dynamic"),
            {"analysisFrames", "2"},
            {"nbrPartials", "1"},
            {"firstPartial", "1"},
        }));
    talas::runtime::RenderSettings settings{16.0, 4};
    settings.tableResources.push_back({
        "Analysis/C102Dynamic",
        {1.0, 1.0, 0.0, 0.5, 2.0, 0.0},
    });
    // SOSOscillators takes any Spectrum: a SpectrumOnDisk,
    // a connected envelope or an Event Value envelope demotes the instance
    // to the subset path instead of failing the compile.
    const auto requireDemoted = [](const talas::runtime::CompileResult& result,
                                    std::string_view context) {
        require(result.ok() && result.graph.has_value(),
            std::string{context} + " should compile");
        const auto plan = result.graph->plan();
        const auto sos = std::ranges::find(
            plan.steps,
            std::string_view{"sos"},
            &talas::runtime::CompileStep::nodeId);
        require(sos != plan.steps.end()
                && sos->offlineSupport == talas::core::ExecutionSupport::Subset
                && sos->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{context} + " should demote SOS Oscillators");
    };
    requireDemoted(talas::runtime::compileProject(
                       diskDocument, makeRegistry(), settings),
        "a SpectrumOnDisk Spectrum");

    auto envelopeDocument = makeSosOscillatorsOutputDocument();
    envelopeDocument.graph.nodes.push_back(
        node("envelope", "talas.constant", {{"value", "1"}}));
    envelopeDocument.graph.connections.push_back(connection(
        "envelope", "value", "spectrum", "envelope"));
    requireDemoted(talas::runtime::compileProject(
                       envelopeDocument, makeRegistry(), settings),
        "a connected Spectrum envelope");

    auto eventSettings = settings;
    eventSettings.eventValues = {{
        "C102Envelope",
        1.0,
        talas::expr::ValueDomain::Scalar,
    }};
    requireDemoted(talas::runtime::compileProject(
                       makeSosOscillatorsOutputDocument(
                           sosOscillatorsParameters(),
                           syntheticSpectrumFromArrayParameters({
                               {"envelope", "!C102Envelope"},
                           })),
                       makeRegistry(),
                       eventSettings),
        "an Event Value Spectrum envelope");
}

void testLowPassGateCompileAuthorityAndConfigurationBounds()
{
    const auto settings = talas::runtime::RenderSettings{1000.0, 8};
    const auto valid = talas::runtime::compileProject(
        makeLowPassGateOutputDocument(), makeRegistry(), settings);
    require(valid.ok(), "C103 bounded Low-Pass Gate graph should compile");
    const auto& gate = requireStep(valid.graph->plan(), "gate");
    require(gate.typeId == "talas.lowPassGate"
            && gate.parameters.size() == 7
            && gate.parameters[0].id == "control"
            && gate.parameters[1].id == "closedFrequency"
            && gate.parameters[2].id == "openFrequency"
            && gate.parameters[3].id == "attackTime"
            && gate.parameters[4].id == "releaseTime"
            && gate.parameters[5].id == "curve"
            && gate.parameters[6].id == "scale"
            && gate.inputs.size() == 1
            && gate.inputs[0].inputPortId == "audioIn"
            && gate.offlineSupport == talas::core::ExecutionSupport::Supported
            && gate.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && gate.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && gate.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && gate.supportsReset
            && gate.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && gate.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{
                    std::size_t{0}}
            && gate.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && gate.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "C103 Low-Pass Gate compile plan should preserve exact ordered lifecycle authority");

    auto scheduledSettings = settings;
    scheduledSettings.eventValues = {{
        "C168Control", 0.5, talas::expr::ValueDomain::Scalar,
    }};
    auto scheduledDocument = makeLowPassGateOutputDocument();
    std::ranges::find(
        scheduledDocument.graph.nodes,
        std::string_view{"gate"},
        &talas::project::ProjectNode::instanceId)->parameters[0].expression =
        "!C168Control";
    const auto scheduled = talas::runtime::compileProject(
        scheduledDocument, makeRegistry(), scheduledSettings);
    require(scheduled.ok(),
        "C168 direct scheduled Low-Pass Gate control should compile");
    const auto& scheduledGate = requireStep(
        scheduled.graph->plan(), "gate");
    require(scheduledGate.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && scheduledGate.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported,
        "C168 direct stateless control schedule should retain promoted authority");

    auto contextualSettings = settings;
    contextualSettings.eventValues = {{
        "C168CutoffOffset", 0.0, talas::expr::ValueDomain::Hertz,
    }};
    auto contextualDocument = makeLowPassGateOutputDocument();
    std::ranges::find(
        contextualDocument.graph.nodes,
        std::string_view{"gate"},
        &talas::project::ProjectNode::instanceId)->parameters[1].expression =
        "10 hz + !C168CutoffOffset";
    const auto contextual = talas::runtime::compileProject(
        contextualDocument, makeRegistry(), contextualSettings);
    require(contextual.ok(),
        "C168 contextual static-configuration fixture should compile");
    const auto& contextualGate = requireStep(
        contextual.graph->plan(), "gate");
    require(contextualGate.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualGate.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C168 contextual configuration must demote both execution axes without minting authority");

    auto statefulDocument = makeLowPassGateOutputDocument();
    std::ranges::find(
        statefulDocument.graph.nodes,
        std::string_view{"gate"},
        &talas::project::ProjectNode::instanceId)->parameters[0].expression =
        "previous(0.5, 0.5)";
    const auto stateful = talas::runtime::compileProject(
        statefulDocument, makeRegistry(), settings);
    require(stateful.ok(),
        "C168 stateful control demotion fixture should compile");
    const auto& statefulGate = requireStep(
        stateful.graph->plan(), "gate");
    require(statefulGate.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && statefulGate.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C168 stateful control must remain both Subset");

    for (const auto& [parameterId, expression] :
         std::array<std::pair<std::string_view, std::string_view>, 8>{
             std::pair{"closedFrequency", "0 hz"},
             std::pair{"openFrequency", "9 hz"},
             std::pair{"openFrequency", "500 hz"},
             std::pair{"attackTime", "-0.01 s"},
             std::pair{"releaseTime", "-0.01 s"},
             std::pair{"curve", "0.24"},
             std::pair{"curve", "8.01"},
             std::pair{"scale", "1 / 0"},
         })
    {
        auto document = makeLowPassGateOutputDocument();
        auto& parameters = std::ranges::find(
            document.graph.nodes,
            std::string_view{"gate"},
            &talas::project::ProjectNode::instanceId)->parameters;
        std::ranges::find(
            parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId)
            ->expression = expression;
        requireCompileFailure(
            talas::runtime::compileProject(
                document, makeRegistry(), settings),
            DiagnosticCode::ParameterEvaluationError,
            "gate",
            parameterId);
    }

    auto connected = makeLowPassGateOutputDocument({
        {"control", "1 / 0"},
        {"closedFrequency", "10 hz"},
        {"openFrequency", "400 hz"},
        {"attackTime", "0.01 s"},
        {"releaseTime", "0.05 s"},
        {"curve", "2"},
        {"scale", "1"},
    });
    connected.graph.nodes.push_back(node(
        "control", "talas.constant", {{"value", "0.75"}}));
    connected.graph.connections.push_back(connection(
        "control", "value", "gate", "control"));
    const auto connectedResult = talas::runtime::compileProject(
        connected, makeRegistry(), settings);
    require(connectedResult.ok(),
        "C103 connected Low-Pass Gate control should own and hide its invalid direct fallback");
    const auto& connectedGate = requireStep(
        connectedResult.graph->plan(), "gate");
    require(connectedGate.inputs.size() == 2
            && connectedGate.inputs[0].inputPortId == "audioIn"
            && connectedGate.inputs[1].inputPortId == "control",
        "C103 connected Low-Pass Gate should preserve exact ordered input authority");
}

void testFilterBankGraphCompilesWithSpectrumAndAudioInput()
{
    const auto result = talas::runtime::compileProject(makeFilterBankOutputDocument(), makeRegistry(), {});

    require(result.ok(), "filter bank graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "filter bank output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "bank", "filter bank output should point to bank node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "filter bank output should point to audioOut");

    const auto& bank = requireStep(plan, "bank");
    require(bank.typeId == "talas.filterBank", "filter bank step should retain node type");
    require(requireInputBinding(bank, "spectrum").sourceNodeId == "spectrum", "filter bank should bind synthetic spectrum");
    require(requireInputBinding(bank, "audioIn").sourceNodeId == "input", "filter bank should bind audio input");
    requireNear(requireParameter(bank, "nbrFilters").value, 2.0, 1.0e-12, "filter bank filter count should compile");
    requireNear(requireParameter(bank, "bankSize").value, 2.0, 1.0e-12, "filter bank size should compile");
    requireNear(requireParameter(bank, "bandwidth").value, 10.0, 1.0e-12, "filter bank bandwidth should compile");
}

void testFilterBankElementGraphCompilesWithSpectrumAudioAndCascadeInput()
{
    const auto result = talas::runtime::compileProject(makeFilterBankElementOutputDocument(), makeRegistry(), {});

    require(result.ok(), "filter bank element graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "filter bank element output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "element", "filter bank element output should point to element node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "filter bank element output should point to audioOut");

    const auto& element = requireStep(plan, "element");
    require(element.typeId == "talas.filterBankElement", "filter bank element step should retain node type");
    require(requireInputBinding(element, "spectrum").sourceNodeId == "spectrum", "filter bank element should bind synthetic spectrum");
    require(requireInputBinding(element, "audioIn").sourceNodeId == "input", "filter bank element should bind audio input");
    require(requireInputBinding(element, "cascadeInput").sourceNodeId == "cascade", "filter bank element should bind cascade input");
    requireNear(requireParameter(element, "first").value, 1.0, 1.0e-12, "filter bank element first should compile");
    requireNear(requireParameter(element, "count").value, 2.0, 1.0e-12, "filter bank element count should compile");
    requireNear(requireParameter(element, "bandwidth").value, 10.0, 1.0e-12, "filter bank element bandwidth should compile");
}

void testFormantBankGraphCompilesWithSpectrumInput()
{
    const auto result = talas::runtime::compileProject(makeFormantBankOutputDocument(), makeRegistry(), {});

    require(result.ok(), "formant bank graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "formant bank output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "bank", "formant bank output should point to bank node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "formant bank output should point to audioOut");

    const auto& bank = requireStep(plan, "bank");
    require(bank.typeId == "talas.formantBank", "formant bank step should retain node type");
    require(
        bank.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && bank.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && bank.supportsReset
            && !bank.kernelKind.has_value()
            && bank.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && bank.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "formant bank should preserve exact C25 prepared authority");
    require(requireInputBinding(bank, "spectrum").sourceNodeId == "spectrum", "formant bank should bind synthetic spectrum");
    requireNear(requireParameter(bank, "frequency").value, 1.0, 1.0e-12, "formant bank frequency should compile");
    requireNear(requireParameter(bank, "nbrFormants").value, 2.0, 1.0e-12, "formant bank formant count should compile");
    requireNear(requireParameter(bank, "bankSize").value, 2.0, 1.0e-12, "formant bank size should compile");
    requireResourceParameter(bank, "impulseResponse", "");
    requireNear(requireParameter(bank, "nbrImpulses").value, 1.0, 1.0e-12, "formant bank overlap count should default to one");
    requireBooleanParameter(bank, "allowDC", false);
}

void testFormantBankNamedImpulseResourcesAndOverlapsCompile()
{
    const auto bankResult = talas::runtime::compileProject(
        makeFormantBankOutputDocument(formantBankParameters({
            {"nbrImpulses", "2"},
            booleanParameter("allowDC", true),
            resourceParameter("impulseResponse", "Tables/FormantImpulse"),
        })),
        makeRegistry(),
        {});
    require(bankResult.ok(), "FormantBank should compile named impulse resources and overlap counts;" + compileDiagnosticsSummary(bankResult));
    const auto& bank = requireStep(bankResult.graph->plan(), "bank");
    requireResourceParameter(bank, "impulseResponse", "Tables/FormantImpulse");
    requireNear(requireParameter(bank, "nbrImpulses").value, 2.0, 1.0e-12, "FormantBank overlap count should compile");
    requireBooleanParameter(bank, "allowDC", true);

    const auto oscillatorResult = talas::runtime::compileProject(
        makeFormantBankOscillatorOutputDocument(formantBankOscillatorParameters({
            {"nbrImpulses", "2"},
            resourceParameter("formantImpulse", "Tables/FormantImpulse"),
        })),
        makeRegistry(),
        {});
    require(oscillatorResult.ok(), "FormantBankOscillator should compile named impulse resources and overlap counts;" + compileDiagnosticsSummary(oscillatorResult));
    const auto& oscillator = requireStep(oscillatorResult.graph->plan(), "formant");
    requireResourceParameter(oscillator, "formantImpulse", "Tables/FormantImpulse");
    requireNear(requireParameter(oscillator, "nbrImpulses").value, 2.0, 1.0e-12, "FormantBankOscillator overlap count should compile");
}

void testFormantBankOscillatorGraphCompilesWithSpectrumCascadeAndAudioOutput()
{
    const auto result = talas::runtime::compileProject(makeFormantBankOscillatorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "formant bank oscillator graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "formant bank oscillator output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "formant", "formant bank oscillator output should point to formant node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "formant bank oscillator output should point to audioOut");

    const auto& formant = requireStep(plan, "formant");
    require(formant.typeId == "talas.formantBankOscillator", "formant bank oscillator step should retain node type");
    require(
        formant.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && formant.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && formant.supportsReset
            && !formant.kernelKind.has_value()
            && formant.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && formant.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "formant bank oscillator should preserve exact C26 prepared authority");
    require(requireInputBinding(formant, "spectrum").sourceNodeId == "spectrum", "formant bank oscillator should bind synthetic spectrum");
    require(requireInputBinding(formant, "cascadeInput").sourceNodeId == "cascade", "formant bank oscillator should bind cascade input");
    requireNear(requireParameter(formant, "frequency").value, 1.0, 1.0e-12, "formant bank oscillator frequency should compile");
    requireNear(requireParameter(formant, "firstFormant").value, 1.0, 1.0e-12, "formant bank oscillator firstFormant should compile");
    requireNear(requireParameter(formant, "nbrFormants").value, 2.0, 1.0e-12, "formant bank oscillator nbrFormants should compile");
    requireResourceParameter(formant, "formantImpulse", "");
    requireNear(requireParameter(formant, "nbrImpulses").value, 1.0, 1.0e-12, "formant bank oscillator nbrImpulses should compile");
    requireBooleanParameter(formant, "allowFormantAtDC", false);
}

void testFormantBanksCompileWithMatchedSerializedPairInputs()
{
    const auto bankResult = talas::runtime::compileProject(
        makeFormantBankSerializedPairOutputDocument(), makeRegistry(), {});
    require(bankResult.ok(), "formant bank should compile matching serialized pair inputs;" + compileDiagnosticsSummary(bankResult));
    const auto& bankPlan = bankResult.graph->plan();
    const auto& bank = requireStep(bankPlan, "bank");
    const auto& bankSpectrum = requireStep(bankPlan, "spectrum");
    const auto& bankAmplitude = requireInputBinding(bank, "amplitudeIn");
    const auto& bankFrequency = requireInputBinding(bank, "frequencyIn");
    require(bankAmplitude.sourceNodeId == bankSpectrum.nodeId
            && bankAmplitude.sourcePortId == "amplitudeOut"
            && bankFrequency.sourceNodeId == bankSpectrum.nodeId
            && bankFrequency.sourcePortId == "frequencyOut"
            && bankAmplitude.sourceScopeId == bankSpectrum.scopeId
            && bankFrequency.sourceScopeId == bankSpectrum.scopeId,
        "formant bank should bind matching serialized outputs from one scoped synthetic array source");

    const auto oscillatorResult = talas::runtime::compileProject(
        makeFormantBankOscillatorSerializedPairOutputDocument(), makeRegistry(), {});
    require(oscillatorResult.ok(), "formant bank oscillator should compile matching serialized pair inputs;" + compileDiagnosticsSummary(oscillatorResult));
    const auto& oscillatorPlan = oscillatorResult.graph->plan();
    const auto& oscillator = requireStep(oscillatorPlan, "formant");
    const auto& oscillatorSpectrum = requireStep(oscillatorPlan, "spectrum");
    const auto& oscillatorAmplitude = requireInputBinding(oscillator, "amplitudeIn");
    const auto& oscillatorFrequency = requireInputBinding(oscillator, "frequencyIn");
    require(oscillatorAmplitude.sourceNodeId == oscillatorSpectrum.nodeId
            && oscillatorAmplitude.sourcePortId == "amplitudeOut"
            && oscillatorFrequency.sourceNodeId == oscillatorSpectrum.nodeId
            && oscillatorFrequency.sourcePortId == "frequencyOut"
            && oscillatorAmplitude.sourceScopeId == oscillatorSpectrum.scopeId
            && oscillatorFrequency.sourceScopeId == oscillatorSpectrum.scopeId,
        "formant bank oscillator should bind matching serialized outputs from one scoped synthetic array source");
    require(requireInputBinding(oscillator, "cascadeInput").sourceNodeId == "cascade",
        "formant bank oscillator should retain its required cascade input with serialized pairs");
}

void testFormantBankOscillatorGraphCompilesWithDcFormantEnabled()
{
    const auto result = talas::runtime::compileProject(
        makeFormantBankOscillatorOutputDocument(formantBankOscillatorParameters({booleanParameter("allowFormantAtDC", true)})),
        makeRegistry(),
        {});

    require(result.ok(), "formant bank oscillator should compile with allowFormantAtDC enabled;" + compileDiagnosticsSummary(result));
    const auto& formant = requireStep(result.graph->plan(), "formant");
    requireBooleanParameter(formant, "allowFormantAtDC", true);
}

void requireVocoderCompilePlan(const talas::runtime::CompileResult& result, std::string_view expectedTypeId)
{
    require(result.ok(), std::string(expectedTypeId) + " graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), std::string(expectedTypeId) + " output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "vocoder", std::string(expectedTypeId) + " output should point to vocoder node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", std::string(expectedTypeId) + " output should point to audioOut");

    const auto& vocoder = requireStep(plan, "vocoder");
    require(vocoder.typeId == expectedTypeId, std::string(expectedTypeId) + " step should retain node type");
    require(requireInputBinding(vocoder, "audioIn").sourceNodeId == "input", std::string(expectedTypeId) + " should bind audioIn");
    require(requireInputBinding(vocoder, "sideChain").sourceNodeId == "side", std::string(expectedTypeId) + " should bind sideChain");
}

void testVocoderGraphsCompileAsRuntimeSubsets()
{
    auto result = talas::runtime::compileProject(makeVocoderOutputDocument("talas.scaleVocoder", scaleVocoderParameters()), makeRegistry(), {});
    requireVocoderCompilePlan(result, "talas.scaleVocoder");
    requireTextParameter(requireStep(result.graph->plan(), "vocoder"), "intervals", "");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "inputLevel").value, 1.0, 1.0e-12, "scale vocoder inputLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "sideLevel").value, 0.0, 1.0e-12, "scale vocoder sideLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "gain").value, 1.0, 1.0e-12, "scale vocoder gain should compile");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.scaleVocoder", scaleVocoderParameters({{"inputLevel", "0.5"}, {"gain", "0.25"}})),
        makeRegistry(),
        {});
    requireVocoderCompilePlan(result, "talas.scaleVocoder");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "inputLevel").value, 0.5, 1.0e-12, "scale vocoder scaled inputLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "gain").value, 0.25, 1.0e-12, "scale vocoder scaled gain should compile");

    auto scaleSettings = talas::runtime::RenderSettings{};
    scaleSettings.sampleRate = 48000.0;
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "4"},
                {"bankSize", "4"},
                {"tonic", "69 nn"},
                textParameter("intervals", "-12 0 7.5 12"),
                {"sideLevel", "1"},
                {"inBandwidth", "0.4"},
                {"sideBandwidth", "0.6"},
            })),
        makeRegistry(),
        scaleSettings);
    requireVocoderCompilePlan(result, "talas.scaleVocoder");
    requireTextParameter(
        requireStep(result.graph->plan(), "vocoder"),
        "intervals",
        "-12 0 7.5 12");
    require(
        requireStep(result.graph->plan(), "vocoder").resolvedLatencyFrames == 0,
        "active ScaleVocoder should declare zero intrinsic latency");

    result = talas::runtime::compileProject(makeVocoderOutputDocument("talas.tunableVocoder", tunableVocoderParameters()), makeRegistry(), {});
    requireVocoderCompilePlan(result, "talas.tunableVocoder");
    requireBooleanParameter(requireStep(result.graph->plan(), "vocoder"), "logSpacing", false);
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "analysisLevel").value, 0.0, 1.0e-12, "tunable vocoder analysisLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "synthesisLevel").value, 1.0, 1.0e-12, "tunable vocoder synthesisLevel should compile");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.tunableVocoder", tunableVocoderParameters({{"synthesisLevel", "0.5"}, {"gain", "0.25"}})),
        makeRegistry(),
        {});
    requireVocoderCompilePlan(result, "talas.tunableVocoder");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "synthesisLevel").value, 0.5, 1.0e-12, "tunable vocoder scaled synthesisLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "gain").value, 0.25, 1.0e-12, "tunable vocoder scaled gain should compile");

    for (const bool logarithmic : {false, true})
    {
        auto tunableSettings = talas::runtime::RenderSettings{};
        tunableSettings.sampleRate = 48000.0;
        result = talas::runtime::compileProject(
            makeVocoderOutputDocument(
                "talas.tunableVocoder",
                tunableVocoderParameters({
                    {"nbrBands", "4"},
                    {"bankSize", "4"},
                    booleanParameter("logSpacing", logarithmic),
                    {"analysisFreq", "300 hz"},
                    {"synthesisFreq", "400 hz"},
                    {"analysisLevel", "1"},
                    {"analysisSpacing", "0.5"},
                    {"synthesisSpacing", "0.25"},
                    {"analysisBW", "0.4"},
                    {"synthesisBW", "0.6"},
                })),
            makeRegistry(),
            tunableSettings);
        requireVocoderCompilePlan(result, "talas.tunableVocoder");
        requireBooleanParameter(
            requireStep(result.graph->plan(), "vocoder"),
            "logSpacing",
            logarithmic);
        require(
            requireStep(result.graph->plan(), "vocoder").resolvedLatencyFrames == 0,
            "active TunableVocoder should declare zero intrinsic latency");
    }

    result = talas::runtime::compileProject(makeVocoderOutputDocument("talas.vocoder", vocoderParameters()), makeRegistry(), {});
    requireVocoderCompilePlan(result, "talas.vocoder");
    requireBooleanParameter(requireStep(result.graph->plan(), "vocoder"), "pitch", false);
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "inputLevel").value, 1.0, 1.0e-12, "vocoder inputLevel should compile");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "sideLevel").value, 0.0, 1.0e-12, "vocoder sideLevel should compile");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.vocoder", vocoderParameters({{"inputLevel", "0.25"}})),
        makeRegistry(),
        {});
    requireVocoderCompilePlan(result, "talas.vocoder");
    requireNear(requireParameter(requireStep(result.graph->plan(), "vocoder"), "inputLevel").value, 0.25, 1.0e-12, "vocoder scaled inputLevel should compile");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.vocoder",
            vocoderParameters({
                {"nbrBands", "65"},
                {"bankSize", "7"},
                {"sideLevel", "0"},
            })),
        makeRegistry(),
        {});
    requireVocoderCompilePlan(result, "talas.vocoder");
    requireNear(
        requireParameter(requireStep(result.graph->plan(), "vocoder"), "nbrBands").value,
        65.0,
        1.0e-12,
        "dry Vocoder should retain legacy nbrBands compatibility");
    requireNear(
        requireParameter(requireStep(result.graph->plan(), "vocoder"), "bankSize").value,
        7.0,
        1.0e-12,
        "dry Vocoder should retain independent legacy bankSize compatibility");

    auto activeSettings = talas::runtime::RenderSettings{};
    activeSettings.sampleRate = 48000.0;
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.vocoder",
            vocoderParameters({
                {"nbrBands", "4"},
                {"bankSize", "4"},
                {"sideLevel", "1"},
                {"lowCF", "300 hz"},
                {"highCF", "3000 hz"},
            })),
        makeRegistry(),
        activeSettings);
    requireVocoderCompilePlan(result, "talas.vocoder");
    requireNear(
        requireParameter(requireStep(result.graph->plan(), "vocoder"), "sideLevel").value,
        1.0,
        1.0e-12,
        "active vocoder sideLevel should compile");
    require(
        requireStep(result.graph->plan(), "vocoder").resolvedLatencyFrames == 0,
        "active linear vocoder should declare zero intrinsic latency");
}

void testVocoderChannelBankGraphCompilesAsActiveMappedBank()
{
    const auto result = talas::runtime::compileProject(makeVocoderChannelBankOutputDocument(), makeRegistry(), {});

    require(result.ok(), "vocoder channel bank graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "vocoder channel bank output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "bank", "vocoder channel bank output should point to bank node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "vocoder channel bank output should point to audioOut");

    const auto& bank = requireStep(plan, "bank");
    require(bank.typeId == "talas.vocoderChannelBank", "vocoder channel bank step should retain node type");
    require(requireInputBinding(bank, "cascadeInput").sourceNodeId == "cascade", "vocoder channel bank should bind cascadeInput");
    require(requireInputBinding(bank, "audioIn").sourceNodeId == "input", "vocoder channel bank should bind audioIn");
    require(requireInputBinding(bank, "sideChain").sourceNodeId == "side", "vocoder channel bank should bind sideChain");
    require(requireInputBinding(bank, "inputParameters").sourceNodeId == "inputParams", "vocoder channel bank should bind inputParameters");
    require(requireInputBinding(bank, "sideChainParameters").sourceNodeId == "sideParams", "vocoder channel bank should bind sideChainParameters");
    requireNear(requireParameter(bank, "first").value, 1.0, 1.0e-12, "vocoder channel bank first should compile");
    requireNear(requireParameter(bank, "count").value, 2.0, 1.0e-12, "vocoder channel bank count should compile");
    require(bank.resolvedLatencyFrames == 0,
        "active vocoder channel bank should declare zero intrinsic latency");
}

void testContextFreeGrammarGraphCompilesAsBlankSilenceSource()
{
    const auto result = talas::runtime::compileProject(makeContextFreeGrammarOutputDocument(), makeRegistry(), {});

    require(result.ok(), "blank context free grammar graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "context free grammar output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "grammar", "context free grammar output should point to grammar node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "context free grammar output should point to audioOut");

    const auto& grammar = requireStep(plan, "grammar");
    require(grammar.typeId == "talas.contextFreeGrammar", "context free grammar step should retain node type");
    requireTextParameter(grammar, "rewriteRules", "");
    requireTextParameter(grammar, "startExpression", "");
    requireNear(requireParameter(grammar, "seed").value, 0.0, 1.0e-12, "context free grammar seed should compile");
    requireNear(requireParameter(grammar, "maxRewrites").value, 0.0, 1.0e-12, "context free grammar maxRewrites should compile");
}

void testLimeInterpreterGraphCompilesAsBlankStereoSource()
{
    const auto result = talas::runtime::compileProject(makeLimeInterpreterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "blank lime interpreter graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "lime left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "lime", "lime left output should point to lime node");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "lime left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "lime right output buffer index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "lime", "lime right output should point to lime node");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "lime right output should point to rightOut");

    const auto& lime = requireStep(plan, "lime");
    require(lime.typeId == "talas.limeInterpreter", "lime interpreter step should retain node type");
    requireResourceParameter(lime, "fileName", "");
    requireTextParameter(lime, "script", "");
    requireNear(requireParameter(lime, "left").value, 0.25, 1.0e-12, "lime left output should compile");
    requireNear(requireParameter(lime, "right").value, -0.5, 1.0e-12, "lime right output should compile");
}

void testFeedbackLoopOutputGraphCompilesAsBlankSilenceSource()
{
    const auto result = talas::runtime::compileProject(makeFeedbackLoopOutputDocument(), makeRegistry(), {});

    require(result.ok(), "blank feedback loop output graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "feedback loop output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "feedback", "feedback loop output should point to feedback node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "feedback loop output should point to audioOut");

    const auto& feedback = requireStep(plan, "feedback");
    require(feedback.typeId == "talas.feedbackLoopOutput", "feedback loop output step should retain node type");
    requireTextParameter(feedback, "connection", "");
    requireNear(requireParameter(feedback, "delay").value, 0.0, 1.0e-12, "feedback loop output delay should compile");
}

void testNamedFeedbackLoopPairCompilesIntoExplicitDelayedComponent()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 1000.0;
    const auto result = talas::runtime::compileProject(
        makePairedFeedbackLoopDocument("loopA", "0 s"),
        makeRegistry(),
        settings);

    require(result.ok(), "named FeedbackLoop pair should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.feedbackPairs.size() == 1, "named FeedbackLoop pair should produce one delayed pair plan");
    const auto& pair = plan.feedbackPairs.front();
    require(pair.connectionId == "loopA", "feedback pair should retain its connection identity");
    require(pair.scopeId == talas::runtime::globalRuntimeScope, "top-level feedback pair should use the global structural scope");
    require(pair.delayFrames == 1, "zero declared feedback delay should clamp to one sample");
    require(plan.steps[pair.outputStepIndex].nodeId == "feedback", "feedback pair should identify its output step");
    require(plan.steps[pair.inputStepIndex].nodeId == "sink", "feedback pair should identify its input commit step");
    require(pair.outputStepIndex < requireStepIndex(plan, "mix"), "feedback read must precede instantaneous component evaluation");
    require(requireStepIndex(plan, "mix") < pair.inputStepIndex, "feedback commit must follow instantaneous component evaluation");
    require(plan.feedbackComponents.size() == 1, "cyclic feedback graph should produce one delayed SCC component");
    require(plan.feedbackComponents.front().feedbackPairIndices == std::vector<std::size_t>{0}, "feedback SCC should reference its delayed pair");
}

void testNamedFeedbackLoopPairValidationRejectsMissingAndDuplicateEndpoints()
{
    auto missingInput = makeFeedbackLoopOutputDocument(feedbackLoopOutputParameters({
        textParameter("connection", "loopA"),
    }));
    auto result = talas::runtime::compileProject(missingInput, makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "feedback", "connection");

    auto missingOutput = makePairedFeedbackLoopDocument();
    std::erase_if(missingOutput.graph.nodes, [](const talas::project::ProjectNode& projectNode) {
        return projectNode.instanceId == "feedback";
    });
    std::erase_if(missingOutput.graph.connections, [](const talas::core::Connection& graphConnection) {
        return graphConnection.fromNode == "feedback";
    });
    missingOutput.graph.connections.push_back(connection("osc", "audioOut", "mix", "in2"));
    result = talas::runtime::compileProject(missingOutput, makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "sink", "connection");

    auto duplicateInput = makePairedFeedbackLoopDocument();
    duplicateInput.graph.nodes.push_back(node("sink2", "talas.feedbackLoopInput", feedbackLoopInputParameters({
        textParameter("connection", "loopA"),
    })));
    duplicateInput.graph.connections.push_back(connection("mix", "audioOut", "sink2", "audioIn"));
    result = talas::runtime::compileProject(duplicateInput, makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "sink2", "connection");

    auto duplicateOutput = makePairedFeedbackLoopDocument();
    duplicateOutput.graph.nodes.push_back(node("feedback2", "talas.feedbackLoopOutput", feedbackLoopOutputParameters({
        textParameter("connection", "loopA"),
    })));
    duplicateOutput.graph.connections.push_back(connection("feedback2", "audioOut", "out", "rightIn"));
    result = talas::runtime::compileProject(duplicateOutput, makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "feedback2", "connection");
}

void testFeedbackLoopRejectsOrdinaryCyclesAndDelayBudgetOverflow()
{
    talas::project::ProjectDocument ordinaryCycle;
    ordinaryCycle.graph.nodes.push_back(node("out", "talas.audioOutput"));
    ordinaryCycle.graph.nodes.push_back(node("first", "talas.vca"));
    ordinaryCycle.graph.nodes.push_back(node("second", "talas.vca"));
    ordinaryCycle.graph.connections.push_back(connection("first", "audioOut", "second", "audioIn"));
    ordinaryCycle.graph.connections.push_back(connection("second", "audioOut", "first", "audioIn"));
    ordinaryCycle.graph.connections.push_back(connection("second", "audioOut", "out", "audioIn"));
    auto result = talas::runtime::compileProject(ordinaryCycle, makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed);

    talas::runtime::RenderSettings constrained;
    constrained.sampleRate = 4.0;
    constrained.maxRealtimeHistoryFrames = 4;
    result = talas::runtime::compileProject(
        makePairedFeedbackLoopDocument("loopA", "2 s"),
        makeRegistry(),
        constrained);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "feedback", "delay");

    const auto dynamicDelay = makePairedFeedbackLoopDocument(
        "loopA",
        "!FeedbackDelay");
    auto dynamicDelaySettings = talas::runtime::RenderSettings{};
    dynamicDelaySettings.eventValues = {
        {"FeedbackDelay", 0.0, talas::expr::ValueDomain::Seconds},
    };
    result = talas::runtime::compileProject(
        dynamicDelay,
        makeRegistry(),
        dynamicDelaySettings);
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "feedback",
        "delay");

    auto sccConstrained = talas::runtime::RenderSettings{};
    sccConstrained.maxFeedbackPlanSteps = 4;
    result = talas::runtime::compileProject(
        makePairedFeedbackLoopDocument(),
        makeRegistry(),
        sccConstrained);
    requireCompileFailure(
        result,
        DiagnosticCode::GraphValidationFailed);

    talas::runtime::RenderSettings exactSizeBoundary;
    exactSizeBoundary.sampleRate = 1.0;
    exactSizeBoundary.maxRealtimeHistoryFrames =
        std::numeric_limits<std::size_t>::max();
    result = talas::runtime::compileProject(
        makePairedFeedbackLoopDocument(
            "loopA",
            "18446744073709551616 s"),
        makeRegistry(),
        exactSizeBoundary);
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "feedback",
        "delay");
}

void testFeedbackLoopCompileOrderIsStableAcrossInsertionPermutations()
{
    auto forward = makePairedFeedbackLoopDocument();
    auto reversed = forward;
    std::ranges::reverse(reversed.graph.nodes);
    std::ranges::reverse(reversed.graph.connections);

    const auto first = talas::runtime::compileProject(forward, makeRegistry(), {});
    const auto second = talas::runtime::compileProject(reversed, makeRegistry(), {});
    require(first.ok() && second.ok(), "feedback insertion-order fixtures should compile");
    const auto& firstPlan = first.graph->plan();
    const auto& secondPlan = second.graph->plan();
    require(firstPlan.steps.size() == secondPlan.steps.size(), "feedback insertion permutations should retain step count");
    for (std::size_t stepIndex = 0; stepIndex < firstPlan.steps.size(); ++stepIndex)
    {
        require(
            firstPlan.steps[stepIndex].nodeId == secondPlan.steps[stepIndex].nodeId,
            "feedback instantaneous order should not depend on graph insertion order");
    }
    require(
        firstPlan.feedbackPairs.size() == 1
            && secondPlan.feedbackPairs.size() == 1
            && firstPlan.feedbackPairs.front().inputStepIndex
                == secondPlan.feedbackPairs.front().inputStepIndex
            && firstPlan.feedbackPairs.front().outputStepIndex
                == secondPlan.feedbackPairs.front().outputStepIndex
            && firstPlan.feedbackComponents.front().stepIndices
                == secondPlan.feedbackComponents.front().stepIndices,
        "feedback pair and SCC metadata should be deterministic across insertion permutations");
}

void testForcedProcessorAssignmentCannotSplitFeedbackComponent()
{
    auto document = makePairedFeedbackLoopDocument();
    document.graph.nodes.push_back(node(
        "placedFeedback",
        "talas.forcedProcessorAssignment",
        {{"processor", "1"}}));
    auto feedbackEdge = std::ranges::find_if(
        document.graph.connections,
        [](const auto& graphConnection) {
            return graphConnection.fromNode == "feedback"
                && graphConnection.toNode == "mix";
        });
    require(
        feedbackEdge != document.graph.connections.end(),
        "feedback placement fixture should expose its feedback edge");
    feedbackEdge->toNode = "placedFeedback";
    feedbackEdge->toPort = "audioIn";
    document.graph.connections.push_back(connection(
        "placedFeedback", "audioOut", "mix", "in2"));

    const auto result =
        talas::runtime::compileProject(document, makeRegistry(), {});
    requireCompileFailure(
        result,
        DiagnosticCode::GraphValidationFailed);
    require(
        result.diagnostics.front().message.find(
            "cannot split one explicit feedback component")
            != std::string::npos,
        "split feedback component should report the exact placement boundary");
}

void testScriptGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeScriptOutputDocument(), makeRegistry(), {});

    require(result.ok(), "blank script graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "script left output index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "script", "script left output should point to script");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "script left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "script right output buffer index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "script", "script right output should point to script");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "script right output should point to rightOut");

    const auto& script = requireStep(plan, "script");
    require(script.typeId == "talas.script", "script step should retain node type");
    require(requireInputBinding(script, "audioIn").sourceNodeId == "osc", "script audioIn should bind oscillator");
    requireTextParameter(script, "script", "");
    requireNear(requireParameter(script, "left").value, 0.5, 1.0e-12, "script left gain should compile");
    requireNear(requireParameter(script, "right").value, -0.25, 1.0e-12, "script right gain should compile");
}

void testScriptGraphCompilesWithoutOptionalAudioInput()
{
    const auto result = talas::runtime::compileProject(makeScriptOutputDocument(scriptParameters(), false), makeRegistry(), {});

    require(result.ok(), "blank script without audioIn should compile");
    const auto& script = requireStep(result.graph->plan(), "script");
    require(
        std::ranges::none_of(script.inputs, [](const talas::runtime::InputBinding& input) { return input.inputPortId == "audioIn"; }),
        "script audioIn should remain optional");
}

void testParameterTransformerGraphCompilesAsBlankPassthrough()
{
    const auto result = talas::runtime::compileProject(makeParameterTransformerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "blank parameter transformer graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "parameter transformer output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "transform", "parameter transformer output buffer should point to transform");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "parameter transformer output buffer should point to audioOut");

    const auto& transform = requireStep(plan, "transform");
    require(transform.typeId == "talas.parameterTransformer", "parameter transformer step should retain node type");
    require(requireInputBinding(transform, "audioIn").sourceNodeId == "osc", "parameter transformer audioIn should bind oscillator");
    requireTextParameter(transform, "transformation", "");
}

void testParameterTransformerRewritesUpstreamParametersBeforeCompile()
{
    const auto result = talas::runtime::compileProject(
        makeParameterTransformerOutputDocument({textParameter(
            "transformation",
            "set frequency = 2 hz\nscale amplitude by 0.25")}),
        makeRegistry(),
        {});

    require(result.ok(), "non-empty parameter transformer graph should compile");
    const auto& plan = result.graph->plan();
    requireNear(requireParameter(requireStep(plan, "osc"), "frequency").value, 2.0, 1.0e-12,
        "parameter transformer should replace the upstream frequency before compile");
    requireNear(requireParameter(requireStep(plan, "osc"), "amplitude").value, 0.25, 1.0e-12,
        "parameter transformer should scale the upstream amplitude before compile");
    requireTextParameter(requireStep(plan, "transform"), "transformation", "");
}

void testPackedFftGraphCompilesWithExactLatencyAndMatrix()
{
    auto result = talas::runtime::compileProject(makeFftOutputDocument(), makeRegistry(), {});

    require(result.ok(), "length-one fft graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "fft output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "fft", "fft output buffer should point to fft");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "fft output buffer should point to audioOut");

    const auto& fft = requireStep(plan, "fft");
    require(fft.typeId == "talas.fft", "fft step should retain node type");
    require(requireInputBinding(fft, "audioIn").sourceNodeId == "osc", "fft audioIn should bind oscillator");
    requireNear(requireParameter(fft, "length").value, 1.0, 1.0e-12, "fft length should compile");
    requireBooleanParameter(fft, "inverse", false);

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "1"}, booleanParameter("inverse", true)}), makeRegistry(), {});
    require(result.ok(), "length-one inverse fft graph should compile");
    requireBooleanParameter(requireStep(result.graph->plan(), "fft"), "inverse", true);

    for (const auto size : {16, 64, 4096})
    {
        result = talas::runtime::compileProject(
            makeFftOutputDocument({
                {"length", std::to_string(size)},
                booleanParameter("inverse", false),
            }),
            makeRegistry(),
            {});
        require(result.ok(),
                "admitted packed FFT size should compile"
                    + compileDiagnosticsSummary(result));
        const auto& admitted = requireStep(result.graph->plan(), "fft");
        require(admitted.resolvedLatencyFrames
                        == static_cast<std::size_t>(size - 1)
                    && admitted.cumulativeInputLatencyFrames == 0
                    && admitted.cumulativeOutputLatencyFrames
                        == static_cast<std::size_t>(size - 1)
                    && result.graph->plan().totalOutputLatencyFrames
                        == static_cast<std::size_t>(size - 1),
                "single packed FFT should publish exact N-1 latency");
    }

    result = talas::runtime::compileProject(
        makePackedFftRoundTripDocument("64"), makeRegistry(), {});
    require(result.ok(),
            "direct packed FFT round trip should compile"
                + compileDiagnosticsSummary(result));
    const auto& forward = requireStep(result.graph->plan(), "forward");
    const auto& inverse = requireStep(result.graph->plan(), "inverse");
    require(forward.resolvedLatencyFrames == 63
                && forward.cumulativeInputLatencyFrames == 0
                && forward.cumulativeOutputLatencyFrames == 63,
            "packed forward should publish exact N-1 cumulative latency");
    require(inverse.resolvedLatencyFrames == 63
                && inverse.cumulativeInputLatencyFrames == 63
                && inverse.cumulativeOutputLatencyFrames == 126,
            "packed inverse should add its independent N-1 latency");
    require(result.graph->plan().criticalPathLatencyFrames == 126
                && result.graph->plan().totalOutputLatencyFrames == 126,
            "direct packed pair should publish exact 2N-2 total latency");

    result = talas::runtime::compileProject(
        makeSerialPackedFftRoundTripDocument(2, "64"),
        makeRegistry(),
        {});
    require(result.ok(),
            "two serial packed FFT round trips should compile"
                + compileDiagnosticsSummary(result));
    require(result.graph->plan().criticalPathLatencyFrames == 252
                && result.graph->plan().totalOutputLatencyFrames == 252,
            "two serial packed pairs should publish exact 4N-4 total latency");
}

void testPackedFftEventLengthRequiresReprepare()
{
    talas::runtime::RenderSettings settings;
    settings.eventValues = {
        {"FftLength", 64.0, talas::expr::ValueDomain::Scalar},
    };
    auto result = talas::runtime::compileProject(
        makeFftOutputDocument({
            {"length", "!FftLength"},
            booleanParameter("inverse", false),
        }),
        makeRegistry(),
        settings);
    require(result.ok(),
            "Event Value-resolved packed FFT length should compile");
    const auto& diagnostics = result.graph->plan().reprepareDiagnostics;
    require(diagnostics.size() == 1
                && diagnostics.front().nodeId == "fft"
                && diagnostics.front().parameterId == "length"
                && diagnostics.front().eventValueName == "FftLength",
            "packed FFT length dependency should require graph reprepare");

    settings.eventValueChanges.push_back({
        1,
        "FftLength",
        128.0,
        talas::expr::ValueDomain::Scalar,
    });
    result = talas::runtime::compileProject(
        makeFftOutputDocument({
            {"length", "!FftLength"},
            booleanParameter("inverse", false),
        }),
        makeRegistry(),
        settings);
    requireCompileFailure(
        result,
        DiagnosticCode::InvalidRenderSettings,
        "fft",
        "length");
}

void testNativeComplexStftGraphCompilesWithExactLatencyAndMatrix()
{
    for (const auto& [window, hop] : std::vector<std::pair<std::string, std::string>>{
             {"rectangular", "64"},
             {"hann", "32"},
             {"hann", "16"},
         })
    {
        talas::runtime::RenderSettings settings;
        settings.maxBlockFrames = 17;
        const auto parameters = nativeStftParameters("64", hop, window);
        const auto result = talas::runtime::compileProject(
            makeNativeStftOutputDocument(parameters, parameters),
            makeRegistry(),
            settings);
        require(result.ok(), "advertised native STFT tuple should compile" + compileDiagnosticsSummary(result));
        const auto& plan = result.graph->plan();
        const auto& forward = requireStep(plan, "forward");
        const auto& inverse = requireStep(plan, "inverse");
        require(forward.typeId == "talas.forwardFftV1"
                    && inverse.typeId == "talas.inverseFftV1",
                "native STFT steps should retain separately versioned identities");
        requireNear(requireParameter(forward, "frameSize").value, 64.0, 0.0,
                    "forward frame size should compile exactly");
        requireNear(requireParameter(forward, "hopSize").value, std::stod(hop), 0.0,
                    "forward hop size should compile exactly");
        requireChoiceParameter(forward, "window", window);
        requireChoiceParameter(inverse, "window", window);
        require(forward.preparedSpectralFrameSize == 64
                    && forward.preparedSpectralHopSize
                        == static_cast<std::size_t>(std::stoull(hop))
                    && inverse.preparedSpectralFrameSize == 64
                    && inverse.preparedSpectralHopSize
                        == static_cast<std::size_t>(std::stoull(hop)),
                "native STFT compile steps should retain exact prepared N/H authority");
        require(requireInputBinding(inverse, "spectrumIn").sourceNodeId == "forward",
                "inverse native STFT should bind the ComplexSpectrum producer");
        const auto spectrum = std::ranges::find_if(
            plan.buffers,
            [](const talas::runtime::BufferSpec& buffer) {
                return buffer.nodeId == "forward"
                    && buffer.portId == "spectrumOut";
            });
        require(spectrum != plan.buffers.end()
                    && spectrum->type == talas::core::PortType::ComplexSpectrum,
                "native forward output should remain a ComplexSpectrum runtime buffer");
        const auto expectedCapacity =
            talas::dsp::preparedSpectralFrameCapacity(
                settings.maxBlockFrames,
                static_cast<std::size_t>(std::stoull(hop)),
                1,
                0);
        require(expectedCapacity.has_value()
                    && spectrum->frames == settings.maxBlockFrames
                    && spectrum->complexFrameCapacity == *expectedCapacity
                    && spectrum->complexMaximumBinsPerFrame == 33
                    && spectrum->complexChannelCount == 1,
                "native forward output should carry its exact bounded sequence shape");
        for (const auto& buffer : plan.buffers)
        {
            if (buffer.type == talas::core::PortType::ComplexSpectrum)
                continue;
            require(buffer.complexFrameCapacity == 0
                        && buffer.complexMaximumBinsPerFrame == 0
                        && buffer.complexChannelCount == 0,
                    "non-complex buffers must not carry ComplexSpectrum shape metadata");
        }
        require(forward.resolvedLatencyFrames == 63
                    && forward.cumulativeInputLatencyFrames == 0
                    && forward.cumulativeOutputLatencyFrames == 63,
                "native forward compile step should resolve exact N-1 latency");
        require(inverse.resolvedLatencyFrames == 0
                    && inverse.cumulativeInputLatencyFrames == 63
                    && inverse.cumulativeOutputLatencyFrames == 63,
                "native inverse should inherit frame arrival latency without adding another N-1");
        require(plan.criticalPathLatencyFrames == 63
                    && plan.totalOutputLatencyFrames == 63,
                "native forward/inverse terminal path should publish exact N-1 total latency");
    }

    const auto defaults = talas::runtime::compileProject(
        makeNativeStftOutputDocument({}, {}), makeRegistry(), {});
    require(defaults.ok(), "native STFT descriptor defaults should form a valid tuple");
    const auto& defaultForward = requireStep(defaults.graph->plan(), "forward");
    requireNear(requireParameter(defaultForward, "frameSize").value, 1024.0, 0.0,
                "native frameSize should default to 1024");
    requireNear(requireParameter(defaultForward, "hopSize").value, 1024.0, 0.0,
                "native hopSize should default to 1024");
    requireChoiceParameter(defaultForward, "window", "rectangular");
}

void testNativeComplexStftRejectsInvalidAndMismatchedPreparedConfiguration()
{
    struct InvalidCase
    {
        std::string frameSize;
        std::string hopSize;
        std::string window;
        std::string parameterId;
    };
    for (const auto& invalid : std::vector<InvalidCase>{
             {"8", "8", "rectangular", "frameSize"},
             {"12", "12", "rectangular", "frameSize"},
             {"4097", "4097", "rectangular", "frameSize"},
             {"64.5", "64", "rectangular", "frameSize"},
             {"nan", "64", "rectangular", "frameSize"},
             {"64", "0", "rectangular", "hopSize"},
             {"64", "16.5", "hann", "hopSize"},
             {"64", "nan", "hann", "hopSize"},
             {"64", "32", "rectangular", "hopSize"},
             {"64", "64", "hann", "hopSize"},
             {"64", "8", "hann", "hopSize"},
         })
    {
        const auto result = talas::runtime::compileProject(
            makeNativeStftOutputDocument(
                nativeStftParameters(
                    invalid.frameSize,
                    invalid.hopSize,
                    invalid.window),
                nativeStftParameters()),
            makeRegistry(),
            {});
        requireCompileFailure(
            result,
            DiagnosticCode::ParameterEvaluationError,
            "forward",
            invalid.parameterId);
    }

    const auto mismatch = talas::runtime::compileProject(
        makeNativeStftOutputDocument(
            nativeStftParameters("64", "32", "hann"),
            nativeStftParameters("64", "16", "hann")),
        makeRegistry(),
        {});
    requireCompileFailure(
        mismatch,
        DiagnosticCode::GraphValidationFailed,
        "inverse",
        "hopSize");
}

void testNativeComplexStftPreparedEventValuesRequireReprepare()
{
    talas::runtime::RenderSettings settings;
    settings.eventValues = {
        {"FrameSize", 64.0, talas::expr::ValueDomain::Scalar},
        {"HopSize", 32.0, talas::expr::ValueDomain::Scalar},
    };
    const auto parameters = nativeStftParameters(
        "!FrameSize", "!HopSize", "hann");
    auto compiled = talas::runtime::compileProject(
        makeNativeStftOutputDocument(parameters, parameters),
        makeRegistry(),
        settings);
    require(compiled.ok(),
            "Event Value-resolved native STFT configuration should compile at its prepared values");
    const auto& diagnostics = compiled.graph->plan().reprepareDiagnostics;
    require(diagnostics.size() == 4,
            "both native nodes should mark frame and hop roots as reprepare-required");
    for (const auto& diagnostic : diagnostics)
    {
        require((diagnostic.nodeId == "forward"
                    || diagnostic.nodeId == "inverse")
                    && (diagnostic.parameterId == "frameSize"
                        || diagnostic.parameterId == "hopSize")
                    && (diagnostic.eventValueName == "FrameSize"
                        || diagnostic.eventValueName == "HopSize"),
                "native STFT reprepare diagnostics should identify exact configuration roots");
    }

    settings.eventValueChanges.push_back({
        1,
        "HopSize",
        16.0,
        talas::expr::ValueDomain::Scalar,
    });
    compiled = talas::runtime::compileProject(
        makeNativeStftOutputDocument(parameters, parameters),
        makeRegistry(),
        settings);
    requireCompileFailure(
        compiled,
        DiagnosticCode::InvalidRenderSettings,
        "forward",
        "hopSize");
}

void testParallelNativeForwardNodesReceiveDistinctComplexBuffers()
{
    const auto parameters = nativeStftParameters(
        "64", "16", "hann");
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("mix", "talas.mixer"),
        node("inverseA", "talas.inverseFftV1", parameters),
        node("inverseB", "talas.inverseFftV1", parameters),
        node("forwardA", "talas.forwardFftV1", parameters),
        node("forwardB", "talas.forwardFftV1", parameters),
        node("oscA", "talas.oscillator", {
            {"frequency", "1 hz"}, {"amplitude", "1.0"}}),
        node("oscB", "talas.oscillator", {
            {"frequency", "2 hz"}, {"amplitude", "0.5"}}),
    };
    document.graph.connections = {
        connection("oscA", "audioOut", "forwardA", "audioIn"),
        connection("forwardA", "spectrumOut", "inverseA", "spectrumIn"),
        connection("inverseA", "audioOut", "mix", "in1"),
        connection("oscB", "audioOut", "forwardB", "audioIn"),
        connection("forwardB", "spectrumOut", "inverseB", "spectrumIn"),
        connection("inverseB", "audioOut", "mix", "in2"),
        connection("mix", "audioOut", "out", "audioIn"),
    };
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 64.0;
    settings.maxBlockFrames = 17;
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    require(result.ok(),
            "parallel native forward nodes should compile");
    const auto& buffers = result.graph->plan().buffers;
    const auto first = std::ranges::find_if(
        buffers,
        [](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == "forwardA"
                && buffer.portId == "spectrumOut";
        });
    const auto second = std::ranges::find_if(
        buffers,
        [](const talas::runtime::BufferSpec& buffer) {
            return buffer.nodeId == "forwardB"
                && buffer.portId == "spectrumOut";
        });
    require(first != buffers.end() && second != buffers.end()
                && first != second
                && first->type == talas::core::PortType::ComplexSpectrum
                && second->type == talas::core::PortType::ComplexSpectrum
                && first->complexFrameCapacity
                    == second->complexFrameCapacity
                && first->complexMaximumBinsPerFrame
                    == second->complexMaximumBinsPerFrame
                && first->complexChannelCount
                    == second->complexChannelCount,
            "parallel native forward nodes should own distinct equally shaped buffer identities");
}

void testSyntheticSpectrumTrackSelectorGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSyntheticSpectrumTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "synthetic spectrum track selector graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "spectrum selector left output index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "selector", "spectrum selector left output should point to selector");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "spectrum selector left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "spectrum selector right output index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "selector", "spectrum selector right output should point to selector");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "spectrum selector right output should point to rightOut");

    const auto spectrumBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "spectrum" && buffer.portId == "spectrumOut";
    });
    require(spectrumBuffer != plan.buffers.end(), "synthetic spectrum should expose a spectrumOut buffer");
    require(spectrumBuffer->type == talas::core::PortType::Spectrum, "synthetic spectrum buffer should be typed as Spectrum");

    const auto& spectrum = requireStep(plan, "spectrum");
    requireNear(requireParameter(spectrum, "nbrPartials").value, 2.0, 1.0e-12, "synthetic spectrum nbrPartials should compile");
    requireBooleanParameter(spectrum, "logScale", false);
    requireBooleanParameter(spectrum, "sendBandwidths", true);
    requireNear(requireParameter(spectrum, "envelope").value, 0.5, 1.0e-12, "synthetic spectrum envelope should compile");
    requireTextParameter(spectrum, "amplitudes", "1.0 0.25");
    requireTextParameter(spectrum, "frequencies", "100 200");
    requireTextParameter(spectrum, "bandwidths", "10 20");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "spectrum", "selector spectrumIn should bind synthetic spectrum");
    requireNear(requireParameter(selector, "track").value, 2.0, 1.0e-12, "selector track should compile");
    requireBooleanParameter(selector, "amp", true);
    requireBooleanParameter(selector, "freq", true);
}

void testSyntheticSpectrumFromSoundsGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "synthetic spectrum from sounds graph should compile");
    const auto& plan = result.graph->plan();

    const auto spectrumBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "spectrum" && buffer.portId == "spectrumOut";
    });
    require(spectrumBuffer != plan.buffers.end(), "synthetic spectrum from sounds should expose a spectrumOut buffer");
    require(spectrumBuffer->type == talas::core::PortType::Spectrum, "synthetic spectrum from sounds buffer should be typed as Spectrum");

    const auto& spectrum = requireStep(plan, "spectrum");
    require(spectrum.typeId == "talas.syntheticSpectrumFromSounds", "synthetic spectrum from sounds step should retain node type");
    require(requireInputBinding(spectrum, "amplitudes").sourceNodeId == "amps", "amplitudes should bind channel 1 audio input");
    require(requireInputBinding(spectrum, "frequenciesAndBandwidths").sourceNodeId == "freqs", "frequencies should bind channel 2 audio input");
    requireNear(requireParameter(spectrum, "nbrPartials").value, 2.0, 1.0e-12, "synthetic spectrum from sounds partial count should compile");
    requireBooleanParameter(spectrum, "logScale", false);
    requireBooleanParameter(spectrum, "includesBandwidths", false);

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "spectrum", "selector should consume synthetic spectrum from sounds output");
}

void testSyntheticSpectrumFromSoundsBandwidthsCompile()
{
    const auto result = talas::runtime::compileProject(
        makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(
            syntheticSpectrumFromSoundsParameters({booleanParameter("includesBandwidths", true)})),
        makeRegistry(),
        {});

    require(result.ok(), "synthetic spectrum from sounds bandwidth mode should compile");
    const auto& spectrum = requireStep(result.graph->plan(), "spectrum");
    requireBooleanParameter(spectrum, "includesBandwidths", true);
}

void testSyntheticSpectrumFromSoundsLogScaleModesCompile()
{
    auto result = talas::runtime::compileProject(
        makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(
            syntheticSpectrumFromSoundsParameters({booleanParameter("logScale", true)})),
        makeRegistry(),
        {});

    require(result.ok(), "synthetic spectrum from sounds logScale frequency-only mode should compile");
    auto const* spectrum = &requireStep(result.graph->plan(), "spectrum");
    requireBooleanParameter(*spectrum, "logScale", true);
    requireBooleanParameter(*spectrum, "includesBandwidths", false);

    result = talas::runtime::compileProject(
        makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(
            syntheticSpectrumFromSoundsParameters({
                booleanParameter("logScale", true),
                booleanParameter("includesBandwidths", true),
            })),
        makeRegistry(),
        {});

    require(result.ok(), "synthetic spectrum from sounds logScale bandwidth mode should compile");
    spectrum = &requireStep(result.graph->plan(), "spectrum");
    requireBooleanParameter(*spectrum, "logScale", true);
    requireBooleanParameter(*spectrum, "includesBandwidths", true);
}

void testLiveSpectralAnalysisGraphCompilesWithSpectrumBuffer()
{
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 17;
    const auto result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(),
        makeRegistry(),
        settings);

    require(result.ok(), "live spectral analysis graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();

    const auto spectrumBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "spectrum" && buffer.portId == "spectrumOut";
    });
    require(spectrumBuffer != plan.buffers.end(), "live spectral analysis should expose a spectrumOut buffer");
    require(spectrumBuffer->type == talas::core::PortType::Spectrum, "live spectral analysis buffer should be typed as Spectrum");
    const auto expectedCapacity =
        talas::dsp::preparedSpectralFrameCapacity(
            settings.maxBlockFrames,
            4,
            1,
            0);
    require(expectedCapacity.has_value()
                && spectrumBuffer->partialFrameCapacity
                    == *expectedCapacity
                && spectrumBuffer->partialMaximumPartialsPerFrame
                    == talas::dsp::maximumLiveSpectralPartials
                && spectrumBuffer->complexFrameCapacity == 0
                && spectrumBuffer->complexMaximumBinsPerFrame == 0
                && spectrumBuffer->complexChannelCount == 0,
            "native live spectral output should carry its exact bounded PartialSpectrumV1 shape");
    for (const auto& buffer : plan.buffers)
    {
        if (&buffer == &*spectrumBuffer)
            continue;
        require(buffer.partialFrameCapacity == 0
                    && buffer.partialMaximumPartialsPerFrame == 0,
                "legacy Spectrum and non-spectrum buffers must not carry PartialSpectrumV1 shape metadata");
    }

    const auto& spectrum = requireStep(plan, "spectrum");
    require(spectrum.typeId == "talas.liveSpectralAnalysis", "live spectral analysis step should retain node type");
    require(requireInputBinding(spectrum, "audioIn").sourceNodeId == "input", "live spectral analysis should bind audio input");
    requireNear(requireParameter(spectrum, "lowestAnalyzedFreq").value, 3000.0, 1.0e-12, "live spectral lowest frequency should compile");
    requireNear(requireParameter(spectrum, "ampScale").value, 0.5, 1.0e-12, "live spectral amplitude scale should compile");
    requireNear(requireParameter(spectrum, "freqScale").value, 1.0, 1.0e-12, "live spectral frequency scale should compile");
    requireTextParameter(spectrum, "response", "");
    requireBooleanParameter(spectrum, "harmonic", false);
    requireNear(requireParameter(spectrum, "unpitchedOnly").value, 0.0, 1.0e-12, "live spectral unpitchedOnly should compile");
    requireNear(requireParameter(spectrum, "trackedHarmonic").value, 1.0, 1.0e-12, "live spectral tracked harmonic should compile");
    requireNear(requireParameter(spectrum, "lowFreq").value, 1000.0, 1.0e-12, "live spectral lowFreq should compile");
    requireNear(requireParameter(spectrum, "highFreq").value, 12000.0, 1.0e-12, "live spectral highFreq should compile");
    requireNear(requireParameter(spectrum, "initFreq").value, 3000.0, 1.0e-12, "live spectral initFreq should compile");
    requireBooleanParameter(spectrum, "fundamentalOnly", true);
    require(spectrum.preparedSpectralFrameSize == 16
                && spectrum.preparedSpectralHopSize == 4
                && spectrum.resolvedLatencyFrames == 15
                && spectrum.cumulativeInputLatencyFrames == 0
                && spectrum.cumulativeOutputLatencyFrames == 15,
            "live spectral analysis should record N=16, H=4, and exact N-1 latency");

    const auto& bridge = requireStep(plan, "bridge");
    require(requireInputBinding(bridge, "spectrumIn").sourceNodeId == "spectrum", "legacy bridge should consume native live spectral analysis frames");
    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "bridge", "selector should consume the explicit legacy hold output");
    requireNear(requireParameter(selector, "track").value, 1.0, 1.0e-12, "selector should read first live spectral partial");
}

void testLiveSpectralAnalysisDerivesExactPreparedFrameBoundaries()
{
    struct ValidCase
    {
        std::string lowestAnalyzedFrequency;
        std::size_t frameSize;
    };
    for (const auto& valid : std::vector<ValidCase>{
             {"3000 hz", 16},
             {"1000.5 hz", 64},
             {"750 hz", 64},
             {"11.71875 hz", 4096},
         })
    {
        auto parameters = liveSpectralAnalysisParameters({
            {"lowestAnalyzedFreq", valid.lowestAnalyzedFrequency},
            {"lowFreq", "20 hz"},
            {"highFreq", "12000 hz"},
            {"initFreq", "1000 hz"},
            booleanParameter("fundamentalOnly", false),
        });
        const auto result = talas::runtime::compileProject(
            makeLiveSpectralAnalysisTrackSelectorOutputDocument(
                std::move(parameters)),
            makeRegistry(),
            {});
        require(
            result.ok(),
            "valid live spectral frame boundary "
                + valid.lowestAnalyzedFrequency
                + " should compile;"
                + compileDiagnosticsSummary(result));
        const auto& step = requireStep(result.graph->plan(), "spectrum");
        require(step.preparedSpectralFrameSize == valid.frameSize
                    && step.preparedSpectralHopSize
                        == valid.frameSize / 4
                    && step.resolvedLatencyFrames
                        == valid.frameSize - 1,
                "live spectral frame boundary should record exact N/H/latency authority");
    }

    const auto impossible = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(
            liveSpectralAnalysisParameters({
                {"lowestAnalyzedFreq", "11 hz"},
                {"lowFreq", "20 hz"},
                {"highFreq", "12000 hz"},
                {"initFreq", "1000 hz"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(
        impossible,
        DiagnosticCode::ParameterEvaluationError,
        "spectrum",
        "lowestAnalyzedFreq");
}

void testLiveSpectralAnalysisCarriesCumulativeLatencyAndReprepareAuthority()
{
    auto result = talas::runtime::compileProject(
        makeLatencyPrefixedLiveSpectralAnalysisOutputDocument(),
        makeRegistry(),
        {});
    require(
        result.ok(),
        "latency-prefixed live spectral graph should compile;"
            + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    const auto& prefix = requireStep(plan, "latencyPrefix");
    const auto& spectrum = requireStep(plan, "spectrum");
    const auto& bridge = requireStep(plan, "bridge");
    require(prefix.cumulativeOutputLatencyFrames == 63
                && spectrum.cumulativeInputLatencyFrames == 63
                && spectrum.resolvedLatencyFrames == 15
                && spectrum.cumulativeOutputLatencyFrames == 78
                && bridge.resolvedLatencyFrames == 0
                && bridge.cumulativeInputLatencyFrames == 78
                && bridge.cumulativeOutputLatencyFrames == 78,
            "live spectral analysis should add N-1 to exact upstream cumulative latency while the hold adds none");

    talas::runtime::RenderSettings settings;
    settings.eventValues = {
        {"LowestFrequency", 750.0, talas::expr::ValueDomain::Hertz},
    };
    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(
            liveSpectralAnalysisParameters({
                {"lowestAnalyzedFreq", "!LowestFrequency"},
            })),
        makeRegistry(),
        settings);
    require(
        result.ok(),
        "Event Value-resolved live spectral resolution should compile;"
            + compileDiagnosticsSummary(result));
    const auto& reprepare = result.graph->plan().reprepareDiagnostics;
    require(reprepare.size() == 1
                && reprepare.front().nodeId == "spectrum"
                && reprepare.front().parameterId
                    == "lowestAnalyzedFreq"
                && reprepare.front().eventValueName
                    == "LowestFrequency",
            "every Event Value dependency that changes live spectral N should require reprepare");

    settings.eventValueChanges.push_back({
        1,
        "LowestFrequency",
        1000.5,
        talas::expr::ValueDomain::Hertz,
    });
    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(
            liveSpectralAnalysisParameters({
                {"lowestAnalyzedFreq", "!LowestFrequency"},
            })),
        makeRegistry(),
        settings);
    requireCompileFailure(
        result,
        DiagnosticCode::InvalidRenderSettings,
        "spectrum",
        "lowestAnalyzedFreq");
}

void testSpectrumInRamGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSpectrumInRamTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum in ram graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();

    const auto sourceBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "source" && buffer.portId == "spectrumOut";
    });
    require(sourceBuffer != plan.buffers.end(), "spectrum in ram should expose a spectrumOut buffer");
    require(sourceBuffer->type == talas::core::PortType::Spectrum, "spectrum in ram output should be typed as Spectrum");

    const auto& source = requireStep(plan, "source");
    require(source.typeId == "talas.spectrumInRam", "spectrum in ram step should retain node type");
    requireNear(requireParameter(source, "frequency").value, 100.0, 1.0e-9, "spectrum in ram frequency should compile");
    requireNear(requireParameter(source, "level").value, 0.5, 1.0e-12, "spectrum in ram level should compile");
    requireNear(requireParameter(source, "timeIndex").value, 0.0, 1.0e-12, "spectrum in ram timeIndex should compile");
    requireResourceParameter(source, "analysis", "Analysis/RAM");
    requireNear(requireParameter(source, "analysisFrames").value, 1.0, 1.0e-12, "spectrum in ram analysisFrames should compile");
    requireNear(requireParameter(source, "nbrPartials").value, 2.0, 1.0e-12, "spectrum in ram partial count should compile");
    requireNear(requireParameter(source, "firstPartial").value, 1.0, 1.0e-12, "spectrum in ram first partial should compile");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "source", "selector should consume spectrum in ram output");
}

void testC59SpectrumInRamCompilesWithDefaultPitchAndPartialCount()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument({
            resourceParameter("analysis", "Analysis/RAM"),
            {"analysisFrequency", "125 hz"},
        }),
        makeRegistry(),
        {});

    require(
        result.ok(),
        "C59 minimally authored SpectrumInRAM should compile without unresolved default identifiers;"
            + compileDiagnosticsSummary(result));
    const auto& source = requireStep(result.graph->plan(), "source");
    requireNear(
        requireParameter(source, "frequency").value,
        0.0,
        0.0,
        "C59 omitted frequency should compile as the static default sentinel");
    requireNear(
        requireParameter(source, "analysisFrequency").value,
        125.0,
        0.0,
        "C59 analysisFrequency should retain project-authored original pitch");
    requireNear(
        requireParameter(source, "nbrPartials").value,
        0.0,
        0.0,
        "C59 omitted nbrPartials should compile as the static default sentinel");
}

void testSpectrumInRamGraphCompilesWithStaticTimeIndex()
{
    for (const auto expected : {-1.0, 0.0, 1.0})
    {
        const auto result = talas::runtime::compileProject(
            makeSpectrumInRamTrackSelectorOutputDocument(
                spectrumInRamParameters({
                    {"timeIndex", std::to_string(expected)},
                })),
            makeRegistry(),
            {});

        require(
            result.ok(),
            "spectrum in ram normalized static timeIndex should compile;"
                + compileDiagnosticsSummary(result));
        const auto& source = requireStep(result.graph->plan(), "source");
        requireNear(
            requireParameter(source, "timeIndex").value,
            expected,
            1.0e-12,
            "spectrum in ram normalized static timeIndex should compile");
    }
}

void testSpectrumInRamGraphCompilesWithFractionalTimeIndex()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"timeIndex", "0.5"}})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum in ram fractional timeIndex should compile;" + compileDiagnosticsSummary(result));
    const auto& source = requireStep(result.graph->plan(), "source");
    requireNear(requireParameter(source, "timeIndex").value, 0.5, 1.0e-12, "spectrum in ram fractional timeIndex should compile");
}

void testSpectrumOnDiskGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSpectrumOnDiskTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum on disk graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();

    const auto sourceBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "source" && buffer.portId == "spectrumOut";
    });
    require(sourceBuffer != plan.buffers.end(), "spectrum on disk should expose a spectrumOut buffer");
    require(sourceBuffer->type == talas::core::PortType::Spectrum, "spectrum on disk output should be typed as Spectrum");

    const auto& source = requireStep(plan, "source");
    require(source.typeId == "talas.spectrumOnDisk", "spectrum on disk step should retain node type");
    requireNear(requireParameter(source, "frequency").value, 100.0, 1.0e-9, "spectrum on disk frequency should compile");
    requireNear(requireParameter(source, "level").value, 0.5, 1.0e-12, "spectrum on disk level should compile");
    requireNear(requireParameter(source, "rateScale").value, 1.0, 1.0e-12, "spectrum on disk rateScale should compile");
    requireNear(requireParameter(source, "trigger").value, 0.0, 1.0e-12, "spectrum on disk trigger should compile");
    requireResourceParameter(source, "fileName", "Analysis/Disk");
    requireNear(requireParameter(source, "analysisFrames").value, 1.0, 1.0e-12, "spectrum on disk analysisFrames should compile");
    requireNear(requireParameter(source, "analysisFrequency").value, 1.0, 1.0e-12, "spectrum on disk analysisFrequency should compile");
    requireNear(requireParameter(source, "nbrPartials").value, 2.0, 1.0e-12, "spectrum on disk partial count should compile");
    requireNear(requireParameter(source, "firstPartial").value, 1.0, 1.0e-12, "spectrum on disk first partial should compile");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "source", "selector should consume spectrum on disk output");
}

void testSpectrumOnDiskHistoricalParametersUseC56Defaults()
{
    auto parameters = spectrumOnDiskParameters();
    std::erase_if(
        parameters,
        [](const talas::project::ProjectParameterExpression& parameter) {
            return parameter.parameterId == "trigger"
                || parameter.parameterId == "analysisFrames";
        });
    const auto result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(
            std::move(parameters)),
        makeRegistry(),
        {});

    require(result.ok(),
            "historical Spectrum On Disk parameters should compile through C56 defaults");
    const auto& source = requireStep(result.graph->plan(), "source");
    requireNear(requireParameter(source, "trigger").value, 0.0, 1.0e-12,
                "historical Spectrum On Disk should default trigger to zero");
    requireNear(requireParameter(source, "analysisFrames").value, 1.0, 1.0e-12,
                "historical Spectrum On Disk should default to one analysis frame");
}

void testC60SpectrumOnDiskCompilesWithDefaultPitchAndPartialCount()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument({
            resourceParameter("fileName", "Analysis/Disk"),
            {"analysisFrames", "2"},
            {"analysisFrequency", "125 hz"},
        }),
        makeRegistry(),
        {});

    require(
        result.ok(),
        "C60 minimally authored SpectrumOnDisk should compile without unresolved default identifiers;"
            + compileDiagnosticsSummary(result));
    const auto& source = requireStep(result.graph->plan(), "source");
    requireNear(
        requireParameter(source, "frequency").value,
        0.0,
        0.0,
        "C60 omitted frequency should compile as the static default sentinel");
    requireNear(
        requireParameter(source, "analysisFrequency").value,
        125.0,
        0.0,
        "C60 analysisFrequency should retain project-authored original pitch");
    requireNear(
        requireParameter(source, "nbrPartials").value,
        0.0,
        0.0,
        "C60 omitted nbrPartials should compile as the static default sentinel");
}

void testSpectrumFrequencyScaleGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSpectrumFrequencyScaleTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum frequency scale graph should compile");
    const auto& plan = result.graph->plan();

    const auto scaleBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "scale" && buffer.portId == "spectrumOut";
    });
    require(scaleBuffer != plan.buffers.end(), "spectrum frequency scale should expose a spectrumOut buffer");
    require(scaleBuffer->type == talas::core::PortType::Spectrum, "spectrum frequency scale output should be typed as Spectrum");

    const auto& scale = requireStep(plan, "scale");
    require(scale.typeId == "talas.spectrumFrequencyScale", "spectrum frequency scale step should retain node type");
    require(requireInputBinding(scale, "spectrumIn").sourceNodeId == "spectrum", "spectrum frequency scale should bind synthetic spectrum");
    requireNear(requireParameter(scale, "scale").value, 0.5, 1.0e-12, "spectrum frequency scale factor should compile");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "scale", "selector should consume scaled spectrum");
}

void testSpectrumLogToLinearGraphCompilesWithSpectrumOutput()
{
    const auto result = talas::runtime::compileProject(makeSpectrumLogToLinearOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum log-to-linear graph should compile");
    const auto& plan = result.graph->plan();
    const auto linearBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "linear" && buffer.portId == "spectrumOut";
    });
    require(linearBuffer != plan.buffers.end(), "spectrum log-to-linear should expose a spectrumOut buffer");
    require(linearBuffer->type == talas::core::PortType::Spectrum, "spectrum log-to-linear output should be typed as Spectrum");

    const auto& linear = requireStep(plan, "linear");
    require(linear.typeId == "talas.spectrumLogToLinear", "spectrum log-to-linear step should retain node type");
    require(requireInputBinding(linear, "spectrumIn").sourceNodeId == "spectrum", "spectrum log-to-linear should bind synthetic spectrum");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "linear", "selector should consume log-to-linear spectrum");
}

void testSpectrumFundamentalGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeSpectrumFundamentalOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum fundamental graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "spectrum fundamental left output index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "fundamental", "spectrum fundamental left output should point to fundamental node");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "spectrum fundamental left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "spectrum fundamental right output index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "fundamental", "spectrum fundamental right output should point to fundamental node");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "spectrum fundamental right output should point to rightOut");

    const auto spectrumBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "spectrum" && buffer.portId == "spectrumOut";
    });
    require(spectrumBuffer != plan.buffers.end(), "synthetic spectrum should expose a spectrumOut buffer for fundamental extraction");
    require(spectrumBuffer->type == talas::core::PortType::Spectrum, "spectrum fundamental input buffer should be typed as Spectrum");

    const auto& fundamental = requireStep(plan, "fundamental");
    require(fundamental.typeId == "talas.spectrumFundamental", "spectrum fundamental step should retain node type");
    require(requireInputBinding(fundamental, "spectrumIn").sourceNodeId == "spectrum", "spectrum fundamental should bind synthetic spectrum");
}

void testSpectrumVoicedUnvoicedGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeSpectrumVoicedUnvoicedOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum voiced/unvoiced graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "spectrum voiced/unvoiced left output index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "voiced", "spectrum voiced/unvoiced left output should point to voiced node");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "spectrum voiced/unvoiced left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "spectrum voiced/unvoiced right output index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "voiced", "spectrum voiced/unvoiced right output should point to voiced node");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "spectrum voiced/unvoiced right output should point to rightOut");

    const auto spectrumBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "spectrum" && buffer.portId == "spectrumOut";
    });
    require(spectrumBuffer != plan.buffers.end(), "synthetic spectrum should expose a spectrumOut buffer for voiced/unvoiced extraction");
    require(spectrumBuffer->type == talas::core::PortType::Spectrum, "spectrum voiced/unvoiced input buffer should be typed as Spectrum");

    const auto& voiced = requireStep(plan, "voiced");
    require(voiced.typeId == "talas.spectrumVoicedUnvoiced", "spectrum voiced/unvoiced step should retain node type");
    require(voiced.supportsReset, "C57 spectrum voiced/unvoiced step should retain reset authority");
    require(voiced.resolvedLatencyFrames == std::optional<std::size_t>{0}, "C57 spectrum voiced/unvoiced step should retain zero intrinsic latency");
    require(voiced.cumulativeInputLatencyFrames == std::optional<std::size_t>{0}, "C57 spectrum voiced/unvoiced input ancestry should remain zero latency");
    require(voiced.cumulativeOutputLatencyFrames == std::optional<std::size_t>{0}, "C57 spectrum voiced/unvoiced output ancestry should remain zero latency");
    require(requireInputBinding(voiced, "spectrumIn").sourceNodeId == "spectrum", "spectrum voiced/unvoiced should bind synthetic spectrum");
}

void testSpectralShapeGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSpectralShapeTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectral shape graph should compile");
    const auto& plan = result.graph->plan();

    const auto shapeBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "shape" && buffer.portId == "spectrumOut";
    });
    require(shapeBuffer != plan.buffers.end(), "spectral shape should expose a spectrumOut buffer");
    require(shapeBuffer->type == talas::core::PortType::Spectrum, "spectral shape output should be typed as Spectrum");

    const auto& shape = requireStep(plan, "shape");
    require(shape.typeId == "talas.spectralShape", "spectral shape step should retain node type");
    requireNear(requireParameter(shape, "frequency").value, 100.0, 1.0e-12, "spectral shape frequency should compile");
    requireNear(requireParameter(shape, "spacing").value, 50.0, 1.0e-12, "spectral shape spacing should compile");
    requireNear(requireParameter(shape, "nbrPartials").value, 3.0, 1.0e-12, "spectral shape partial count should compile");
    requireResourceParameter(shape, "wavetable", "Tables/SpectralShape");
    requireNear(requireParameter(shape, "scale").value, 2.0, 1.0e-12, "spectral shape scale should compile");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "shape", "selector should consume spectral shape");
}

void testSpectrumModifierGraphCompilesWithSpectrumBuffer()
{
    const auto result = talas::runtime::compileProject(makeSpectrumModifierTrackSelectorOutputDocument(), makeRegistry(), {});

    require(result.ok(), "spectrum modifier graph should compile"
        + (result.diagnostics.empty() ? std::string{}
            : ": " + result.diagnostics.front().message));
    const auto& plan = result.graph->plan();

    const auto modifierBuffer = std::ranges::find_if(plan.buffers, [](const talas::runtime::BufferSpec& buffer) {
        return buffer.nodeId == "modifier" && buffer.portId == "spectrumOut";
    });
    require(modifierBuffer != plan.buffers.end(), "spectrum modifier should expose a spectrumOut buffer");
    require(modifierBuffer->type == talas::core::PortType::Spectrum, "spectrum modifier output should be typed as Spectrum");

    const auto& modifier = requireStep(plan, "modifier");
    require(modifier.typeId == "talas.spectrumModifier", "spectrum modifier step should retain node type");
    require(
        modifier.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && modifier.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && modifier.supportsReset
            && !modifier.kernelKind.has_value()
            && modifier.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && modifier.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{
                    std::size_t{0}}
            && modifier.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && modifier.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "spectrum modifier should expose exact C52 prepared reset authority");
    require(
        modifier.parameters.size() == 19
            && modifier.parameters[0].id == "select"
            && modifier.parameters[1].id == "loTrack"
            && modifier.parameters[2].id == "hiTrack"
            && modifier.parameters[3].id == "loFreq"
            && modifier.parameters[4].id == "hiFreq"
            && modifier.parameters[5].id == "freqHysteresis"
            && modifier.parameters[6].id == "freqScale"
            && modifier.parameters[7].id == "freqOffset"
            && modifier.parameters[8].id == "loAmp"
            && modifier.parameters[9].id == "hiAmp"
            && modifier.parameters[10].id == "ampHysteresis"
            && modifier.parameters[11].id == "ampScale"
            && modifier.parameters[12].id == "ampOffset"
            && modifier.parameters[13].id == "probability"
            && modifier.parameters[14].id == "seed"
            && modifier.parameters[15].id == "hearAll"
            && modifier.parameters[16].id == "freqSmoothing"
            && modifier.parameters[17].id == "ampSmoothing"
            && modifier.parameters[18].id == "freqOffsetUnits",
        "spectrum modifier should preserve exact C27 parameter order");
    require(requireInputBinding(modifier, "spectrumIn").sourceNodeId == "spectrum", "spectrum modifier should bind synthetic spectrum");
    require(
        modifier.inputs.size() == 1,
        "an unconnected Spectrum Modifier should retain its byte-stable static input plan");
    requireBooleanParameter(modifier, "select", true);
    requireNear(requireParameter(modifier, "loTrack").value, 1.0, 1.0e-12, "spectrum modifier loTrack should compile");
    requireNear(requireParameter(modifier, "hiTrack").value, 2.0, 1.0e-12, "spectrum modifier hiTrack should compile");
    requireNear(requireParameter(modifier, "freqScale").value, 0.5, 1.0e-12, "spectrum modifier freqScale should compile");
    requireNear(requireParameter(modifier, "freqOffset").value, 10.0, 1.0e-12, "spectrum modifier freqOffset should compile");
    requireNear(requireParameter(modifier, "ampScale").value, 2.0, 1.0e-12, "spectrum modifier ampScale should compile");
    requireNear(requireParameter(modifier, "ampOffset").value, 0.25, 1.0e-12, "spectrum modifier ampOffset should compile");
    requireNear(requireParameter(modifier, "probability").value, 1.0, 1.0e-12, "spectrum modifier probability should compile");
    require(
        modifier.spectrumModifierProbabilityProfile.empty(),
        "a scalar C53 probability should retain an empty indexed profile");
    requireBooleanParameter(modifier, "hearAll", false);
    requireNear(requireParameter(modifier, "freqSmoothing").value, 0.0,
        1.0e-12, "spectrum modifier frequency smoothing should default to bypass");
    requireNear(requireParameter(modifier, "ampSmoothing").value, 0.0,
        1.0e-12, "spectrum modifier amplitude smoothing should default to bypass");

    const auto& selector = requireStep(plan, "selector");
    require(requireInputBinding(selector, "spectrumIn").sourceNodeId == "modifier", "selector should consume modified spectrum");
}

void testSpectrumModifierGraphCompilesWithIndependentLiveSelectors()
{
    auto document = makeSpectrumModifierTrackSelectorOutputDocument(
        syntheticSpectrumFromArrayParameters(),
        spectrumModifierParameters({
            booleanParameter("select", false),
            booleanParameter("hearAll", true),
        }));
    document.graph.nodes.push_back(node(
        "selectControl", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "hearAllControl", "talas.constant", {{"value", "0"}}));
    document.graph.connections.push_back(connection(
        "selectControl", "value", "modifier", "select"));
    document.graph.connections.push_back(connection(
        "hearAllControl", "value", "modifier", "hearAll"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});

    require(result.ok(), "spectrum modifier live selectors should compile");
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    require(
        modifier.inputs.size() == 3
            && modifier.inputs[0].inputPortId == "spectrumIn"
            && modifier.inputs[1].inputPortId == "select"
            && modifier.inputs[2].inputPortId == "hearAll",
        "spectrum modifier should preserve exact descriptor-ordered selector bindings");
    const auto& select = requireInputBinding(modifier, "select");
    const auto& hearAll = requireInputBinding(modifier, "hearAll");
    require(
        select.sourceNodeId == "selectControl"
            && select.sourcePortId == "value",
        "select should preserve its exact Control producer");
    require(
        hearAll.sourceNodeId == "hearAllControl"
            && hearAll.sourcePortId == "value",
        "hearAll should preserve its exact Control producer");
    requireBooleanParameter(modifier, "select", false);
    requireBooleanParameter(modifier, "hearAll", true);
    require(
        modifier.parameters.size() == 19,
        "live selectors should retain the exact SpectrumModifier parameter profile");
    std::string latencyProfile;
    for (const auto latency : modifier.inputLatencyFrames)
    {
        if (!latencyProfile.empty())
            latencyProfile += ",";
        latencyProfile += latency.has_value()
            ? std::to_string(*latency)
            : "none";
    }
    require(
        modifier.inputLatencyFrames
            == std::vector<std::optional<std::size_t>>{
                std::size_t{0}, std::nullopt, std::nullopt}
            && modifier.cumulativeInputLatencyFrames == std::size_t{0}
            && modifier.cumulativeOutputLatencyFrames == std::size_t{0},
        "live selectors should retain exact zero-latency authority; got "
            + latencyProfile);

    auto selectOnlyDocument =
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({
                booleanParameter("select", false),
                booleanParameter("hearAll", true),
            }));
    selectOnlyDocument.graph.nodes.push_back(node(
        "selectControl", "talas.constant", {{"value", "-1"}}));
    selectOnlyDocument.graph.connections.push_back(connection(
        "selectControl", "value", "modifier", "select"));
    const auto selectOnly = talas::runtime::compileProject(
        selectOnlyDocument, makeRegistry(), {});
    require(selectOnly.ok(), "a partial live selector binding should compile");
    const auto& partialModifier = requireStep(
        selectOnly.graph->plan(), "modifier");
    require(
        partialModifier.inputs.size() == 2
            && partialModifier.inputs[0].inputPortId == "spectrumIn"
            && partialModifier.inputs[1].inputPortId == "select",
        "a partial selector binding should retain canonical descriptor order");
    requireBooleanParameter(partialModifier, "select", false);
    requireBooleanParameter(partialModifier, "hearAll", true);

    auto duplicateDocument = selectOnlyDocument;
    duplicateDocument.graph.nodes.push_back(node(
        "duplicateSelect", "talas.constant", {{"value", "1"}}));
    duplicateDocument.graph.connections.push_back(connection(
        "duplicateSelect", "value", "modifier", "select"));
    const auto duplicate = talas::runtime::compileProject(
        duplicateDocument, makeRegistry(), {});
    requireCompileFailure(
        duplicate, DiagnosticCode::DuplicateInputConnection, "modifier");

    auto wrongTypeDocument =
        makeSpectrumModifierTrackSelectorOutputDocument();
    wrongTypeDocument.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "modifier", "select"));
    const auto wrongType = talas::runtime::compileProject(
        wrongTypeDocument, makeRegistry(), {});
    requireCompileFailure(
        wrongType, DiagnosticCode::GraphValidationFailed, "modifier");
}

void testSpectrumModifierGraphCompilesWithLiveSeed()
{
    auto document = makeSpectrumModifierTrackSelectorOutputDocument(
        syntheticSpectrumFromArrayParameters(),
        spectrumModifierParameters({{"seed", "-0.75"}}));
    document.graph.nodes.push_back(node(
        "selectControl", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "seedControl", "talas.constant", {{"value", "0.25"}}));
    document.graph.nodes.push_back(node(
        "hearAllControl", "talas.constant", {{"value", "0"}}));
    document.graph.connections.push_back(connection(
        "selectControl", "value", "modifier", "select"));
    document.graph.connections.push_back(connection(
        "seedControl", "value", "modifier", "seed"));
    document.graph.connections.push_back(connection(
        "hearAllControl", "value", "modifier", "hearAll"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});

    require(result.ok(), "Spectrum Modifier live seed should compile");
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    require(
        modifier.inputs.size() == 4
            && modifier.inputs[0].inputPortId == "spectrumIn"
            && modifier.inputs[1].inputPortId == "select"
            && modifier.inputs[2].inputPortId == "seed"
            && modifier.inputs[3].inputPortId == "hearAll",
        "live seed should retain exact descriptor order between probability and hearAll");
    const auto& seed = requireInputBinding(modifier, "seed");
    require(
        seed.sourceNodeId == "seedControl"
            && seed.sourcePortId == "value",
        "seed should preserve its exact Control producer");
    requireNear(
        requireParameter(modifier, "seed").value,
        -0.75,
        1.0e-12,
        "connected seed should retain the exact static fallback parameter");
    require(
        modifier.parameters.size() == 19
            && modifier.parameters[14].id == "seed"
            && modifier.parameters[15].id == "hearAll"
            && modifier.parameters[16].id == "freqSmoothing"
            && modifier.parameters[17].id == "ampSmoothing",
        "live seed should retain the exact SpectrumModifier parameter profile");
    require(
        modifier.inputLatencyFrames
            == std::vector<std::optional<std::size_t>>{
                std::size_t{0}, std::nullopt, std::nullopt, std::nullopt}
            && modifier.cumulativeInputLatencyFrames == std::size_t{0}
            && modifier.cumulativeOutputLatencyFrames == std::size_t{0},
        "live seed should retain exact zero-latency authority");

    auto seedOnlyDocument =
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"seed", "1.0"}}));
    seedOnlyDocument.graph.nodes.push_back(node(
        "seedControl", "talas.constant", {{"value", "-1"}}));
    seedOnlyDocument.graph.connections.push_back(connection(
        "seedControl", "value", "modifier", "seed"));
    const auto seedOnly = talas::runtime::compileProject(
        seedOnlyDocument, makeRegistry(), {});
    require(seedOnly.ok(), "a seed-only live binding should compile");
    const auto& seedOnlyModifier = requireStep(
        seedOnly.graph->plan(), "modifier");
    require(
        seedOnlyModifier.inputs.size() == 2
            && seedOnlyModifier.inputs[0].inputPortId == "spectrumIn"
            && seedOnlyModifier.inputs[1].inputPortId == "seed",
        "a seed-only binding should retain canonical descriptor order");
    requireNear(
        requireParameter(seedOnlyModifier, "seed").value,
        1.0,
        1.0e-12,
        "a seed-only binding should retain its upper-bound fallback");

    auto duplicateDocument = seedOnlyDocument;
    duplicateDocument.graph.nodes.push_back(node(
        "duplicateSeed", "talas.constant", {{"value", "0"}}));
    duplicateDocument.graph.connections.push_back(connection(
        "duplicateSeed", "value", "modifier", "seed"));
    const auto duplicate = talas::runtime::compileProject(
        duplicateDocument, makeRegistry(), {});
    requireCompileFailure(
        duplicate, DiagnosticCode::DuplicateInputConnection, "modifier");

    auto wrongTypeDocument =
        makeSpectrumModifierTrackSelectorOutputDocument();
    wrongTypeDocument.graph.connections.push_back(connection(
        "spectrum", "spectrumOut", "modifier", "seed"));
    const auto wrongType = talas::runtime::compileProject(
        wrongTypeDocument, makeRegistry(), {});
    requireCompileFailure(
        wrongType, DiagnosticCode::GraphValidationFailed, "modifier");
}

void testSpectrumModifierProbabilityAndSeedCompile()
{
    auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"probability", "0.5"}, {"seed", "-0.5"}})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum modifier fractional probability should compile");
    const auto& fractionalModifier = requireStep(result.graph->plan(), "modifier");
    requireNear(requireParameter(fractionalModifier, "probability").value, 0.5, 1.0e-12, "fractional probability should compile");
    requireNear(requireParameter(fractionalModifier, "seed").value, -0.5, 1.0e-12, "negative seed should compile inside range");

    result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"probability", "2.0"}, {"seed", "1.0"}})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum modifier probability above one should compile for render-time clipping");
    const auto& clippedModifier = requireStep(result.graph->plan(), "modifier");
    requireNear(requireParameter(clippedModifier, "probability").value, 2.0, 1.0e-12, "above-one probability should compile");
    requireNear(requireParameter(clippedModifier, "seed").value, 1.0, 1.0e-12, "upper-bound seed should compile");
}

void testSpectrumModifierTrackNumberProbabilityProfileCompiles()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({
                {"probability", "TrackNumber / 4"},
            })),
        makeRegistry(),
        {});

    require(
        result.ok(),
        "Spectrum Modifier should compile an exact TrackNumber probability profile"
            + compileDiagnosticsSummary(result));
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    const auto& probability = requireParameter(modifier, "probability");
    require(
        probability.expression.has_value()
            && probability.expression->identifiers()
                == std::vector<std::string>{"TrackNumber"}
            && !probability.expression->isStateful()
            && probability.expression->eventReferences().empty()
            && probability.expression->variableReferences().empty(),
        "the compiled probability parameter should retain exact static TrackNumber expression authority");
    require(
        modifier.spectrumModifierProbabilityProfile.size()
            == 4096,
        "the compiled TrackNumber probability profile should cover every admitted source ordinal");
    requireNear(
        probability.value,
        0.25,
        1.0e-12,
        "the scalar compatibility value should equal the clipped first profile entry");
    requireNear(
        modifier.spectrumModifierProbabilityProfile[0],
        0.25,
        1.0e-12,
        "track one probability should compile");
    requireNear(
        modifier.spectrumModifierProbabilityProfile[1],
        0.5,
        1.0e-12,
        "track two probability should compile");
    requireNear(
        modifier.spectrumModifierProbabilityProfile[2],
        0.75,
        1.0e-12,
        "track three probability should compile");
    requireNear(
        modifier.spectrumModifierProbabilityProfile[3],
        1.0,
        1.0e-12,
        "track four probability should compile");
    requireNear(
        modifier.spectrumModifierProbabilityProfile.back(),
        1.0,
        1.0e-12,
        "above-one late track probability should be clipped to one");

    auto definitionDocument =
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({
                {"probability", "trackProbability(TrackNumber)"},
            }));
    definitionDocument.expressionDefinitions.definitions.push_back({
        "trackProbability",
        {{"track", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "track / 4",
    });
    const auto definitionExpanded = talas::runtime::compileProject(
        definitionDocument,
        makeRegistry(),
        {});
    require(
        definitionExpanded.ok(),
        "definition-expanded TrackNumber probability should compile"
            + compileDiagnosticsSummary(definitionExpanded));
    const auto& definitionModifier = requireStep(
        definitionExpanded.graph->plan(), "modifier");
    require(
        definitionModifier.spectrumModifierProbabilityProfile.size()
                == 4096
            && definitionModifier
                    .spectrumModifierProbabilityProfile[2]
                == 0.75
            && definitionModifier
                    .spectrumModifierProbabilityProfile.back()
                == 1.0,
        "definition expansion should retain exact bounded TrackNumber profile authority");

    const auto compileWith = [](std::string expression) {
        return talas::runtime::compileProject(
            makeSpectrumModifierTrackSelectorOutputDocument(
                syntheticSpectrumFromArrayParameters(),
                spectrumModifierParameters({
                    {"probability", std::move(expression)},
                })),
            makeRegistry(),
            {});
    };

    // A profile negative for the last track is effectively zero there
    //, clipped as one above 1 is.
    const auto lateNegative = compileWith(
        "1 - TrackNumber / 4095");
    require(lateNegative.ok(),
        "a TrackNumber probability negative for the last track should compile"
            + compileDiagnosticsSummary(lateNegative));
    const auto& clipped = requireStep(lateNegative.graph->plan(), "modifier")
                              .spectrumModifierProbabilityProfile;
    require(clipped.size() == 4096 && clipped.back() == 0.0
            && clipped[4093] == 1.0 - 4094.0 / 4095.0,
        "a negative TrackNumber probability should clip to zero");
    const auto lateDivisionByZero = compileWith(
        "1 / (4096 - TrackNumber)");
    requireCompileFailure(
        lateDivisionByZero,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
    const auto wrongCase = compileWith("trackNumber / 4");
    requireCompileFailure(
        wrongCase,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
    const auto extraIdentifier = compileWith(
        "TrackNumber + Missing");
    requireCompileFailure(
        extraIdentifier,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
    const auto eventReference = compileWith(
        "TrackNumber / 4 + !ProbabilityOffset");
    requireCompileFailure(
        eventReference,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");

    auto variableDocument =
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({
                {"probability", "TrackNumber / 4 + @offset"},
            }));
    variableDocument.variableLibrary.variables.push_back({
        "offset",
        "Offset",
        talas::expr::ValueDomain::Scalar,
        0.0,
    });
    const auto variableReference = talas::runtime::compileProject(
        variableDocument,
        makeRegistry(),
        {});
    requireCompileFailure(
        variableReference,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");

    const auto stateful = compileWith(
        "previous(TrackNumber, 1)");
    requireCompileFailure(
        stateful,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
    const auto wrongDomain = compileWith(
        "TrackNumber * 1 hz");
    requireCompileFailure(
        wrongDomain,
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
}

void testSpectrumModifierVariablesV2RemainderProbabilityProfileCompiles()
{
    auto document = makeSpectrumModifierTrackSelectorOutputDocument(
        syntheticSpectrumFromArrayParameters(),
        spectrumModifierParameters({
            {"probability", "(TrackNumber - 1) rem: 2"},
        }));
    require(
        document.expressionLanguage
            == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "C55 remainder fixture should use exact VariablesV2 authority");

    const auto result = talas::runtime::compileProject(
        document,
        makeRegistry(),
        {});
    require(
        result.ok(),
        "C55 historical remainder probability profile should compile"
            + compileDiagnosticsSummary(result));
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    const auto& probability = requireParameter(modifier, "probability");
    require(
        probability.expression.has_value()
            && probability.expression->identifiers()
                == std::vector<std::string>{"TrackNumber"}
            && modifier.spectrumModifierProbabilityProfile.size() == 4096
            && probability.value == 0.0
            && !std::signbit(probability.value),
        "C55 remainder profile should publish complete immutable TrackNumber authority with positive-zero compatibility value");
    for (std::size_t index = 0;
         index < modifier.spectrumModifierProbabilityProfile.size();
         ++index)
    {
        require(
            modifier.spectrumModifierProbabilityProfile[index]
                == (index % 2 == 0 ? 0.0 : 1.0),
            "C55 remainder profile should alternate exactly for all 4096 one-based source ordinals");
    }
    require(
        modifier.spectrumModifierProbabilityProfile[0] == 0.0
            && modifier.spectrumModifierProbabilityProfile[1] == 1.0
            && modifier.spectrumModifierProbabilityProfile[4094] == 0.0
            && modifier.spectrumModifierProbabilityProfile[4095] == 1.0,
        "C55 remainder profile should preserve exact ordinals 1, 2, 4095, and 4096");

    auto definitionDocument =
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({
                {"probability", "alternatingTrack(TrackNumber)"},
            }));
    definitionDocument.expressionDefinitions.definitions.push_back({
        "alternatingTrack",
        {{"track", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "(track - 1) rem: 2",
    });
    const auto definitionExpanded = talas::runtime::compileProject(
        definitionDocument,
        makeRegistry(),
        {});
    require(
        definitionExpanded.ok(),
        "C55 definition-expanded remainder probability should compile"
            + compileDiagnosticsSummary(definitionExpanded));
    const auto& definitionModifier = requireStep(
        definitionExpanded.graph->plan(), "modifier");
    require(
        definitionModifier.spectrumModifierProbabilityProfile
            == modifier.spectrumModifierProbabilityProfile,
        "C55 definition expansion should publish the exact same immutable alternating profile");

    auto variablesV1 = document;
    variablesV1.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    requireCompileFailure(
        talas::runtime::compileProject(variablesV1, makeRegistry(), {}),
        DiagnosticCode::ParameterParseError,
        "modifier",
        "probability");

    const auto compileWith = [](std::string expression) {
        return talas::runtime::compileProject(
            makeSpectrumModifierTrackSelectorOutputDocument(
                syntheticSpectrumFromArrayParameters(),
                spectrumModifierParameters({
                    {"probability", std::move(expression)},
                })),
            makeRegistry(),
            {});
    };
    requireCompileFailure(
        compileWith("TrackNumber rem: 0"),
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
    requireCompileFailure(
        compileWith("TrackNumber rem: 1 hz"),
        DiagnosticCode::ParameterEvaluationError,
        "modifier",
        "probability");
}

void testSpectrumModifierFrequencyHysteresisCompiles()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"freqHysteresis", "1 hz"}})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum modifier frequency hysteresis should compile");
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    requireNear(requireParameter(modifier, "freqHysteresis").value, 1.0, 1.0e-12, "frequency hysteresis should compile");
}

void testSpectrumModifierAmplitudeHysteresisCompiles()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"ampHysteresis", "0.1"}})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum modifier amplitude hysteresis should compile");
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    requireNear(requireParameter(modifier, "ampHysteresis").value, 0.1, 1.0e-12, "amplitude hysteresis should compile");
}

void testSpectrumModifierRejectModeCompiles()
{
    const auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({booleanParameter("select", false)})),
        makeRegistry(),
        {});

    require(result.ok(), "spectrum modifier reject mode should compile");
    const auto& modifier = requireStep(result.graph->plan(), "modifier");
    requireBooleanParameter(modifier, "select", false);
}

void testSampleGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSampleOutputDocument(), makeRegistry(), {});

    require(result.ok(), "sample graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "sample output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "sample", "sample output buffer should point to sample");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "sample output buffer should point to audioOut");

    const auto& sample = requireStep(plan, "sample");
    requireNear(requireParameter(sample, "frequency").value, 440.0, 1.0e-9, "sample frequency should compile");
    requireNear(requireParameter(sample, "gate").value, 1.0, 1.0e-12, "sample gate should compile");
    requireResourceParameter(sample, "sample", "Samples/Hit");
    requireBooleanParameter(sample, "setLoop", false);
    requireNear(requireParameter(sample, "loopStart").value, 0.0, 1.0e-12, "sample loopStart should compile");
    requireNear(requireParameter(sample, "loopEnd").value, 1.0, 1.0e-12, "sample loopEnd should compile");
    requireBooleanParameter(sample, "loopFade", false);
    requireNear(requireParameter(sample, "start").value, 0.0, 1.0e-12, "sample start should compile");
    requireNear(requireParameter(sample, "end").value, 1.0, 1.0e-12, "sample end should compile");
    requireBooleanParameter(sample, "fromMemoryWriter", false);
    requireNear(requireParameter(sample, "attackTime").value, 0.0, 1.0e-12, "sample attackTime should compile");
    requireNear(requireParameter(sample, "releaseTime").value, 0.0, 1.0e-12, "sample releaseTime should compile");
    requireNear(requireParameter(sample, "scale").value, 0.75, 1.0e-12, "sample scale should compile");
    require(
        sample.parameters.size() == 15
            && sample.parameters[0].id == "frequency"
            && sample.parameters[1].id == "gate"
            && sample.parameters[2].id == "sample"
            && sample.parameters[3].id == "setLoop"
            && sample.parameters[4].id == "loopStart"
            && sample.parameters[5].id == "loopEnd"
            && sample.parameters[6].id == "loopFade"
            && sample.parameters[7].id == "start"
            && sample.parameters[8].id == "end"
            && sample.parameters[9].id == "fromMemoryWriter"
            && sample.parameters[10].id == "attackTime"
            && sample.parameters[11].id == "releaseTime"
            && sample.parameters[12].id == "scale"
            && sample.parameters[13].id == "headerLoop"
            && sample.parameters[13].choice == std::optional<std::string>{"loop"}
            && sample.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && sample.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sample.supportsReset
            && !sample.kernelKind.has_value()
            && sample.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && sample.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && sample.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !sample.preparedIsolatedComponent,
        "sample compile plan should preserve exact C34 prepared authority");
}

void testSampleGraphCompilesWithReversePlaybackRange()
{
    const auto result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "1.0"}, {"end", "0.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "0 s"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});

    require(result.ok(), "reverse sample graph should compile");
    const auto& sample = requireStep(result.graph->plan(), "sample");
    requireNear(requireParameter(sample, "start").value, 1.0, 1.0e-12, "reverse sample start should compile");
    requireNear(requireParameter(sample, "end").value, 0.0, 1.0e-12, "reverse sample end should compile");
}

void testSampleGraphCompilesWithNegativeFrequency()
{
    const auto result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "-440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "0.0"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "0 s"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});

    require(result.ok(), "negative-frequency sample graph should compile");
    const auto& sample = requireStep(result.graph->plan(), "sample");
    requireNear(requireParameter(sample, "frequency").value, -440.0, 1.0e-9, "negative sample frequency should compile");
}

void testDiskPlayerGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeDiskPlayerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "disk player graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "disk player output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "disk", "disk player output buffer should point to disk");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "disk player output buffer should point to audioOut");

    const auto& disk = requireStep(plan, "disk");
    require(disk.typeId == "talas.diskPlayer", "disk player step should retain node type");
    require(disk.inputs.empty(), "disk player should not require inputs");
    requireResourceParameter(disk, "fileName", "Samples/DiskFile");
    requireNear(requireParameter(disk, "filePosition").value, 0.25, 1.0e-12, "disk player filePosition should compile");
    requireNear(requireParameter(disk, "duration").value, 0.5, 1.0e-12, "disk player duration should compile");
    requireNear(requireParameter(disk, "rateScale").value, 1.0, 1.0e-12, "disk player rateScale should compile");
    requireNear(requireParameter(disk, "trigger").value, 1.0, 1.0e-12, "disk player trigger should compile");
}

void testDiskCacheGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeDiskCacheOutputDocument(), makeRegistry(), {});

    require(result.ok(), "disk cache graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "disk cache output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "cache", "disk cache output buffer should point to cache");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "disk cache output buffer should point to audioOut");

    const auto& cache = requireStep(plan, "cache");
    require(cache.typeId == "talas.diskCache", "disk cache step should retain node type");
    requireResourceParameter(cache, "fileName", "Caches/Offline");
    requireBooleanParameter(cache, "record", false);
    require(requireInputBinding(cache, "audioIn").sourceNodeId == "osc", "disk cache audioIn should bind oscillator");
}

void testDiskRecorderGraphCompilesAsTerminalCapture()
{
    const auto result = talas::runtime::compileProject(makeDiskRecorderTerminalDocument(), makeRegistry(), {});

    require(result.ok(), "disk recorder terminal graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "disk recorder terminal buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "osc", "disk recorder terminal buffer should point to oscillator");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "disk recorder terminal buffer should point to oscillator audioOut");
    require(plan.outputBufferIndices.size() == 1, "disk recorder should expose one captured mono output buffer");
    require(plan.outputBufferIndices.front() == plan.outputBufferIndex, "disk recorder outputBufferIndices should mirror legacy output index");

    const auto& recorder = requireStep(plan, "recorder");
    require(recorder.typeId == "talas.diskRecorder", "disk recorder step should retain node type");
    requireResourceParameter(recorder, "fileName", "Recordings/Capture");
    requireTextParameter(recorder, "format", "AIFF");
    requireTextParameter(recorder, "wordSize", "24");
    requireChoiceParameter(recorder, "channels", "mono");
    requireNear(requireParameter(recorder, "trigger").value, 1.0, 1.0e-12, "disk recorder trigger should compile");
    requireNear(requireParameter(recorder, "captureDuration").value, 0.0, 1.0e-12, "disk recorder captureDuration should compile");
    requireBooleanParameter(recorder, "gated", false);
    require(requireInputBinding(recorder, "audioIn").sourceNodeId == "osc", "disk recorder audioIn should bind oscillator");
    require(plan.diskRecorderCapture.has_value(),
        "disk recorder terminal should publish immutable capture authority");
    const auto& capture = *plan.diskRecorderCapture;
    require(capture.stepIndex == static_cast<std::size_t>(&recorder - plan.steps.data())
            && capture.captureBufferIndex == plan.outputBufferIndex
            && !capture.triggerBufferIndex.has_value()
            && !capture.captureDurationBufferIndex.has_value(),
        "disk recorder authority should bind the exact terminal step and capture buffer");
    require(capture.destinationNameHint == "Recordings/Capture"
            && capture.format == "AIFF"
            && capture.wordSize == "24"
            && capture.channels == "mono"
            && capture.trigger == 1.0
            && capture.captureDurationSeconds == 0.0
            && !capture.gated
            && capture.writeSupport
                == talas::runtime::DiskRecorderWriteSupport::Writable,
        "an AIFF/24 recorder should be writable, its Format one of the standard sample formats");
}

void testDiskRecorderWritableProfileCompilesCanonicalAuthority()
{
    const auto result = talas::runtime::compileProject(
        makeDiskRecorderTerminalDocument(diskRecorderParameters({
            textParameter("format", "WAV"),
            textParameter("wordSize", "16"),
        })),
        makeRegistry(),
        {});

    require(result.ok(), "writable WAV16 disk recorder should compile");
    require(result.graph->plan().diskRecorderCapture.has_value(),
        "writable disk recorder should publish capture authority");
    const auto& capture = *result.graph->plan().diskRecorderCapture;
    require(capture.writeSupport
                == talas::runtime::DiskRecorderWriteSupport::Writable
            && capture.format == "WAV"
            && capture.wordSize == "16"
            && capture.channels == "mono",
        "WAV16 mono recorder should publish the canonical writable profile");
}

// A DiskRecorder records in any of its
// formats, SDI, SDII, AIFF, IRCAM and WAVE, at 8, 16 or 24 bits, so each
// compiles to a writable profile: SDII on a Macintosh only, SDI in mono
// only, a Sound Designer I file holding one channel; any other format stays
// a preview.
void testDiskRecorderRecordsInEveryTalasFormat()
{
    const auto support = [](std::string format, std::string channels, std::string wordSize) {
        const auto result = talas::runtime::compileProject(
            makeDiskRecorderTerminalDocument(diskRecorderParameters({
                textParameter("format", format),
                textParameter("wordSize", wordSize),
                choiceParameter("channels", channels),
            })),
            makeRegistry(),
            {});
        require(result.ok() && result.graph->plan().diskRecorderCapture.has_value(),
            "a " + format + " DiskRecorder should compile");
        return result.graph->plan().diskRecorderCapture->writeSupport;
    };
    using talas::runtime::DiskRecorderWriteSupport;
    for (const auto* format : {"SDI", "AIFF", "IRCAM", "WAVE", "WAV"})
    {
        for (const auto* wordSize : {"8", "16", "24"})
        {
            require(support(format, "mono", wordSize) == DiskRecorderWriteSupport::Writable,
                std::string{"a mono "} + format + " DiskRecorder of " + wordSize + " bits should record");
        }
    }
    for (const auto* format : {"AIFF", "IRCAM", "WAVE"})
    {
        require(support(format, "stereo", "16") == DiskRecorderWriteSupport::Writable,
            std::string{"a stereo "} + format + " DiskRecorder should record");
    }
#if defined(__APPLE__)
    require(support("SDII", "stereo", "24") == DiskRecorderWriteSupport::Writable,
        "a stereo SDII DiskRecorder should record on a Macintosh");
#else
    require(support("SDII", "stereo", "24") == DiskRecorderWriteSupport::UnsupportedFormat,
        "SD-II files are a Macintosh's only");
#endif
    require(support("SDI", "stereo", "16") == DiskRecorderWriteSupport::UnsupportedChannels,
        "a Sound Designer I file holds one channel");
    require(support("MP3", "mono", "16") == DiskRecorderWriteSupport::UnsupportedFormat
            && support("IRCAM", "mono", "32") == DiskRecorderWriteSupport::UnsupportedWordSize,
        "an unknown format, or 32 bits, should stay a preview");
}

void testInvalidDiskRecorderParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeDiskRecorderTerminalDocument(diskRecorderParameters({{"captureDuration", "-1 s"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "recorder", "captureDuration");

    result = talas::runtime::compileProject(makeDiskRecorderTerminalDocument(diskRecorderParameters({{"trigger", "1 / 0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "recorder", "trigger");
}

void testMemoryWriterGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeMemoryWriterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "memory writer graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "memory writer output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "writer", "memory writer output buffer should point to writer");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "memory writer output buffer should point to audioOut");

    const auto& writer = requireStep(plan, "writer");
    require(writer.typeId == "talas.memoryWriter", "memory writer step should retain node type");
    requireNear(requireParameter(writer, "captureDuration").value, 1.0, 1.0e-12, "memory writer captureDuration should compile");
    requireBooleanParameter(writer, "global", false);
    requireBooleanParameter(writer, "cyclic", false);
    requireResourceParameter(writer, "recordingName", "Memory/Capture");
    requireBooleanParameter(writer, "silent", false);
    requireNear(requireParameter(writer, "trigger").value, 1.0, 1.0e-12, "memory writer trigger should compile");
    require(requireInputBinding(writer, "audioIn").sourceNodeId == "osc", "memory writer audioIn should bind oscillator");
    require(
        writer.parameters.size() == 6
            && writer.parameters[0].id == "captureDuration"
            && writer.parameters[1].id == "global"
            && writer.parameters[2].id == "cyclic"
            && writer.parameters[3].id == "recordingName"
            && writer.parameters[4].id == "silent"
            && writer.parameters[5].id == "trigger"
            && writer.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && writer.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && writer.supportsReset
            && !writer.kernelKind.has_value()
            && writer.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && writer.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && writer.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !writer.preparedIsolatedComponent,
        "memory writer compile plan should preserve exact C38 prepared authority");
}

void testSampleCloudGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSampleCloudOutputDocument(), makeRegistry(), {});

    require(result.ok(), "sample cloud graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "sample cloud output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "cloud", "sample cloud output buffer should point to cloud");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "sample cloud output buffer should point to audioOut");

    const auto& cloud = requireStep(plan, "cloud");
    require(cloud.typeId == "talas.sampleCloud", "sample cloud step should retain node type");
    require(cloud.inputs.empty(), "sample cloud should not require inputs");
    requireResourceParameter(cloud, "sample", "Samples/Cloud");
    requireResourceParameter(cloud, "grainEnv", "Envelopes/Flat");
    requireNear(requireParameter(cloud, "maxGrains").value, 4.0, 1.0e-12, "sample cloud maxGrains should compile");
    requireNear(requireParameter(cloud, "amplitude").value, 0.5, 1.0e-12, "sample cloud amplitude should compile");
    requireNear(requireParameter(cloud, "density").value, 1.0, 1.0e-12, "sample cloud density should compile");
    requireNear(requireParameter(cloud, "grainDur").value, 1.0, 1.0e-12, "sample cloud grainDur should compile");
    requireNear(requireParameter(cloud, "grainDurJitter").value, 0.0, 1.0e-12, "sample cloud grainDurJitter should compile");
    requireNear(requireParameter(cloud, "pan").value, 0.5, 1.0e-12, "sample cloud pan should compile");
    requireNear(requireParameter(cloud, "panJitter").value, 0.0, 1.0e-12, "sample cloud panJitter should compile");
    requireNear(requireParameter(cloud, "seed").value, 0.0, 1.0e-12, "sample cloud seed should compile");
    requireBooleanParameter(cloud, "fromMemoryWriter", false);
    requireNear(requireParameter(cloud, "timeIndex").value, -1.0, 1.0e-12, "sample cloud timeIndex should compile");
    requireNear(requireParameter(cloud, "timeIndexJitter").value, 0.0, 1.0e-12, "sample cloud timeIndexJitter should compile");
}

void testInvalidSampleCloudParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeSampleCloudOutputDocument(sampleCloudParameters({{"maxGrains", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "maxGrains");

    result = talas::runtime::compileProject(makeSampleCloudOutputDocument(sampleCloudParameters({{"density", "-1"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "density");

    result = talas::runtime::compileProject(makeSampleCloudOutputDocument(sampleCloudParameters({{"grainDur", "0 s"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainDur");

    result = talas::runtime::compileProject(makeSampleCloudOutputDocument(sampleCloudParameters({{"pan", "1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "pan");

    result = talas::runtime::compileProject(makeSampleCloudOutputDocument(sampleCloudParameters({{"timeIndex", "1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "timeIndex");
}

void testGrainCloudGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeGrainCloudOutputDocument(), makeRegistry(), {});

    require(result.ok(), "grain cloud graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "grain cloud left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "cloud", "grain cloud left output should point to cloud");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "grain cloud left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "grain cloud right output buffer index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "cloud", "grain cloud right output should point to cloud");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "grain cloud right output should point to rightOut");

    const auto& cloud = requireStep(plan, "cloud");
    require(cloud.typeId == "talas.grainCloud", "grain cloud step should retain node type");
    requireResourceParameter(cloud, "waveform", "Tables/GrainWave");
    requireResourceParameter(cloud, "grainEnv", "Tables/GrainEnv");
    requireNear(requireParameter(cloud, "maxGrains").value, 4.0, 1.0e-12, "grain cloud maxGrains should compile");
    requireNear(requireParameter(cloud, "amplitude").value, 0.5, 1.0e-12, "grain cloud amplitude should compile");
    requireNear(requireParameter(cloud, "density").value, 1.0, 1.0e-12, "grain cloud density should compile");
    requireNear(requireParameter(cloud, "grainDur").value, 1.0, 1.0e-12, "grain cloud grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "grain cloud cyclesPerGrain should compile");
    requireNear(requireParameter(cloud, "frequency").value, 1.0, 1.0e-12, "grain cloud frequency should compile");
    requireNear(requireParameter(cloud, "pan").value, 0.5, 1.0e-12, "grain cloud pan should compile");
}

void testGrainCloudGraphCompilesWithCycleDerivedDuration()
{
    const auto result = talas::runtime::compileProject(
        makeGrainCloudOutputDocument(grainCloudParameters({{"grainDur", "0 s"}, {"cyclesPerGrain", "1.0"}, {"frequency", "2 hz"}})),
        makeRegistry(),
        {});

    require(result.ok(), "grain cloud graph with cycle-derived duration should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireNear(requireParameter(cloud, "grainDur").value, 0.0, 1.0e-12, "grain cloud zero grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "grain cloud positive cyclesPerGrain should compile");
    requireNear(requireParameter(cloud, "frequency").value, 2.0, 1.0e-12, "grain cloud cycle-derived frequency should compile");
}

void testInvalidGrainCloudParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"maxGrains", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "maxGrains");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"density", "-1"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "density");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"grainDur", "-1 s"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainDur");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"grainDur", "0 s"}, {"cyclesPerGrain", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainDur");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"cyclesPerGrain", "-1"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "cyclesPerGrain");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"frequency", "-1 hz"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "frequency");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"pan", "1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "pan");

    result = talas::runtime::compileProject(makeGrainCloudOutputDocument(grainCloudParameters({{"seed", "1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "seed");
}

void testCloudBankGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeCloudBankOutputDocument(), makeRegistry(), {});

    require(result.ok(), "cloud bank graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "cloud bank left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "cloud", "cloud bank left output should point to cloud");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "cloud bank left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "cloud bank right output buffer index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "cloud", "cloud bank right output should point to cloud");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "cloud bank right output should point to rightOut");

    const auto& cloud = requireStep(plan, "cloud");
    require(cloud.typeId == "talas.cloudBank", "cloud bank step should retain node type");
    require(requireInputBinding(cloud, "trigger").sourceNodeId == "trigger", "cloud bank should bind trigger");
    require(requireInputBinding(cloud, "pan").sourceNodeId == "pan", "cloud bank should bind pan");
    require(requireInputBinding(cloud, "freqMod").sourceNodeId == "freqMod", "cloud bank should bind freqMod");
    require(requireInputBinding(cloud, "grainDurMod").sourceNodeId == "grainDurMod", "cloud bank should bind grainDurMod");
    require(requireInputBinding(cloud, "spectrumSource").sourceNodeId == "spectrum", "cloud bank should bind spectrumSource");
    requireNear(requireParameter(cloud, "nbrTracks").value, 2.0, 1.0e-12, "cloud bank nbrTracks should compile");
    requireNear(requireParameter(cloud, "grainsPerTrack").value, 1.0, 1.0e-12, "cloud bank grainsPerTrack should compile");
    requireNear(requireParameter(cloud, "bankSize").value, 2.0, 1.0e-12, "cloud bank bankSize should compile");
    requireResourceParameter(cloud, "grainWave", "Tables/CloudWave");
    requireResourceParameter(cloud, "grainEnvelope", "Tables/CloudEnv");
    requireNear(requireParameter(cloud, "grainDur").value, 1.0, 1.0e-12, "cloud bank grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "cloud bank cyclesPerGrain should compile");
    requireNear(requireParameter(cloud, "seed").value, 0.0, 1.0e-12, "cloud bank seed should compile");
    requireChoiceParameter(cloud, "interpolation", "linear");
}

void testCloudBankGraphCompilesWithZeroCyclesPerGrain()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankOutputDocument(cloudBankParameters({{"cyclesPerGrain", "0"}})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank graph with zero cyclesPerGrain should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 0.0, 1.0e-12, "cloud bank zero cyclesPerGrain should compile");
}

void testCloudBankGraphCompilesWithCycleDerivedDuration()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankOutputDocument(cloudBankParameters({{"grainDur", "0 s"}, {"cyclesPerGrain", "1"}})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank graph with cycle-derived duration should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireNear(requireParameter(cloud, "grainDur").value, 0.0, 1.0e-12, "cloud bank zero grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "cloud bank positive cyclesPerGrain should compile");
}

void testCloudBankGraphCompilesWithNearestInterpolation()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankOutputDocument(cloudBankParameters({choiceParameter("interpolation", "none")})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank nearest interpolation graph should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireChoiceParameter(cloud, "interpolation", "none");
}

void testCloudBankElementGraphCompilesWithCascadeAndStereoOutputs()
{
    const auto result = talas::runtime::compileProject(makeCloudBankElementOutputDocument(), makeRegistry(), {});

    require(result.ok(), "cloud bank element graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "cloud bank element left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "cloud", "cloud bank element left output should point to cloud");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "cloud bank element left output should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "cloud bank element right output buffer index should be present");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "cloud", "cloud bank element right output should point to cloud");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "cloud bank element right output should point to rightOut");

    const auto& cloud = requireStep(plan, "cloud");
    require(cloud.typeId == "talas.cloudBankElement", "cloud bank element step should retain node type");
    require(requireInputBinding(cloud, "cascadeInput").sourceNodeId == "cascade", "cloud bank element should bind cascadeInput");
    require(requireInputBinding(cloud, "trigger").sourceNodeId == "trigger", "cloud bank element should bind trigger");
    require(requireInputBinding(cloud, "pan").sourceNodeId == "pan", "cloud bank element should bind pan");
    require(requireInputBinding(cloud, "freqMod").sourceNodeId == "freqMod", "cloud bank element should bind freqMod");
    require(requireInputBinding(cloud, "grainDurMod").sourceNodeId == "grainDurMod", "cloud bank element should bind grainDurMod");
    require(requireInputBinding(cloud, "spectrumSource").sourceNodeId == "spectrum", "cloud bank element should bind spectrumSource");
    requireNear(requireParameter(cloud, "first").value, 1.0, 1.0e-12, "cloud bank element first should compile");
    requireNear(requireParameter(cloud, "count").value, 2.0, 1.0e-12, "cloud bank element count should compile");
    requireResourceParameter(cloud, "grainWave", "Tables/CloudWave");
    requireResourceParameter(cloud, "grainEnvelope", "Tables/CloudEnv");
    requireNear(requireParameter(cloud, "grainDur").value, 1.0, 1.0e-12, "cloud bank element grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "cloud bank element cyclesPerGrain should compile");
    requireNear(requireParameter(cloud, "maxGrains").value, 4.0, 1.0e-12, "cloud bank element maxGrains should compile");
    requireNear(requireParameter(cloud, "seed").value, 0.0, 1.0e-12, "cloud bank element seed should compile");
    requireNear(requireParameter(cloud, "initialDelay").value, 0.0, 1.0e-12, "cloud bank element initialDelay should compile");
    requireChoiceParameter(cloud, "interpolation", "linear");
}

void testCloudBankElementGraphCompilesWithZeroCyclesPerGrain()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankElementOutputDocument(cloudBankElementParameters({{"cyclesPerGrain", "0"}})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank element graph with zero cyclesPerGrain should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 0.0, 1.0e-12, "cloud bank element zero cyclesPerGrain should compile");
}

void testCloudBankElementGraphCompilesWithCycleDerivedDuration()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankElementOutputDocument(cloudBankElementParameters({{"grainDur", "0 s"}, {"cyclesPerGrain", "1"}})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank element graph with cycle-derived duration should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireNear(requireParameter(cloud, "grainDur").value, 0.0, 1.0e-12, "cloud bank element zero grainDur should compile");
    requireNear(requireParameter(cloud, "cyclesPerGrain").value, 1.0, 1.0e-12, "cloud bank element positive cyclesPerGrain should compile");
}

void testCloudBankElementGraphCompilesWithNearestInterpolation()
{
    const auto result = talas::runtime::compileProject(
        makeCloudBankElementOutputDocument(cloudBankElementParameters({choiceParameter("interpolation", "none")})),
        makeRegistry(),
        {});

    require(result.ok(), "cloud bank element nearest interpolation graph should compile;" + compileDiagnosticsSummary(result));
    const auto& cloud = requireStep(result.graph->plan(), "cloud");
    requireChoiceParameter(cloud, "interpolation", "none");
}

void testInvalidCloudBankParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeCloudBankOutputDocument(cloudBankParameters({{"nbrTracks", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "nbrTracks");

    result = talas::runtime::compileProject(makeCloudBankOutputDocument(cloudBankParameters({{"grainsPerTrack", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainsPerTrack");

    result = talas::runtime::compileProject(makeCloudBankOutputDocument(cloudBankParameters({{"bankSize", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "bankSize");

    result = talas::runtime::compileProject(makeCloudBankOutputDocument(cloudBankParameters({{"grainDur", "-1 s"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainDur");

    result = talas::runtime::compileProject(makeCloudBankOutputDocument(cloudBankParameters({{"grainDur", "0 s"}, {"cyclesPerGrain", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "grainDur");

}

void testInvalidCloudBankElementParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeCloudBankElementOutputDocument(cloudBankElementParameters({{"first", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "first");

    result = talas::runtime::compileProject(makeCloudBankElementOutputDocument(cloudBankElementParameters({{"count", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "count");

    result = talas::runtime::compileProject(makeCloudBankElementOutputDocument(cloudBankElementParameters({{"maxGrains", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "maxGrains");

    result = talas::runtime::compileProject(makeCloudBankElementOutputDocument(cloudBankElementParameters({{"initialDelay", "-1 s"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "cloud", "initialDelay");
}

void testMultifileDiskPlayerGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeMultifileDiskPlayerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "multifile disk player graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "multifile disk player output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "multiDisk", "multifile disk player output buffer should point to multiDisk");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "multifile disk player output buffer should point to audioOut");

    const auto& multiDisk = requireStep(plan, "multiDisk");
    require(multiDisk.typeId == "talas.multifileDiskPlayer", "multifile disk player step should retain node type");
    require(multiDisk.inputs.empty(), "multifile disk player should not require inputs");
    requireTextParameter(multiDisk, "fileNames", "Samples/A Samples/B");
    requireNear(requireParameter(multiDisk, "rateScale").value, 2.0, 1.0e-12, "multifile disk player rateScale should compile");
    requireNear(requireParameter(multiDisk, "trigger").value, 1.0, 1.0e-12, "multifile disk player trigger should compile");
    requireBooleanParameter(multiDisk, "gated", false);
    requireNear(requireParameter(multiDisk, "index").value, 1.0, 1.0e-12, "multifile disk player index should compile");
    require(
        multiDisk.parameters.size() == 6
            && multiDisk.parameters[0].id == "fileNames"
            && multiDisk.parameters[1].id == "rateScale"
            && multiDisk.parameters[2].id == "trigger"
            && multiDisk.parameters[3].id == "gated"
            && multiDisk.parameters[4].id == "index"
            && multiDisk.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && multiDisk.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multiDisk.supportsReset
            && !multiDisk.kernelKind.has_value()
            && multiDisk.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && multiDisk.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && multiDisk.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !multiDisk.preparedIsolatedComponent,
        "multifile disk player compile plan should preserve exact C37 prepared authority");
}

void testMultifileDiskPlayerNegativeIndexCompilesForRuntimeClamping()
{
    const auto result = talas::runtime::compileProject(
        makeMultifileDiskPlayerOutputDocument(multifileDiskPlayerParameters({{"index", "-1"}})),
        makeRegistry(),
        {});

    require(result.ok(), "multifile disk player negative index should compile for runtime clamping");
    const auto& multiDisk = requireStep(result.graph->plan(), "multiDisk");
    requireNear(requireParameter(multiDisk, "index").value, -1.0, 1.0e-12, "multifile disk player negative index should compile");
}

void testMultifileDiskPlayerGatedCompilesForRuntimeGateControl()
{
    const auto result = talas::runtime::compileProject(
        makeMultifileDiskPlayerOutputDocument(multifileDiskPlayerParameters({booleanParameter("gated", true)})),
        makeRegistry(),
        {});

    require(result.ok(), "gated multifile disk player should compile for runtime gate control");
    const auto& multiDisk = requireStep(result.graph->plan(), "multiDisk");
    requireBooleanParameter(multiDisk, "gated", true);
}

void testConnectedMultifileDiskPlayerControlsOwnUnevaluableFallbacks()
{
    auto document = makeMultifileDiskPlayerOutputDocument(
        multifileDiskPlayerParameters({
            {"rateScale", "1 / 0"},
            {"trigger", "1 / 0"},
            {"index", "1 / 0"},
        }));
    document.graph.nodes.push_back(
        node("index", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(
        node("trigger", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(
        node("rateScale", "talas.constant", {{"value", "0.5"}}));
    document.graph.connections.push_back(
        connection("index", "value", "multiDisk", "index"));
    document.graph.connections.push_back(
        connection("trigger", "value", "multiDisk", "trigger"));
    document.graph.connections.push_back(
        connection("rateScale", "value", "multiDisk", "rateScale"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});

    require(result.ok(),
        "connected Multifile Disk Player controls should make unevaluable direct fallbacks inert");
    const auto& multiDisk = requireStep(result.graph->plan(), "multiDisk");
    require(
        multiDisk.inputs.size() == 3
            && multiDisk.inputs[0].inputPortId == "index"
            && multiDisk.inputs[1].inputPortId == "trigger"
            && multiDisk.inputs[2].inputPortId == "rateScale",
        "connected Multifile Disk Player authority should retain canonical input order");
}

void testSamplesFromDiskSingleStepGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSamplesFromDiskSingleStepOutputDocument(), makeRegistry(), {});

    require(result.ok(), "samples from disk single step graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "samples from disk output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "disk", "samples from disk output buffer should point to disk");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "samples from disk output buffer should point to audioOut");

    const auto& disk = requireStep(plan, "disk");
    require(disk.typeId == "talas.samplesFromDiskSingleStep", "samples from disk step should retain node type");
    require(disk.inputs.empty(), "samples from disk single step should not require inputs");
    requireResourceParameter(disk, "fileName", "Samples/DiskStep");
    requireNear(requireParameter(disk, "filePosition").value, 1.0, 1.0e-12, "samples from disk filePosition should compile");
    requireNear(requireParameter(disk, "trigger").value, 1.0, 1.0e-12, "samples from disk trigger should compile");
    requireNear(requireParameter(disk, "gate").value, 1.0, 1.0e-12, "samples from disk gate should compile");
}

void testMultisampleGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeMultisampleOutputDocument(), makeRegistry(), {});

    require(result.ok(), "multisample graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "multisample output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "multi", "multisample output buffer should point to multi");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "multisample output buffer should point to audioOut");

    const auto& multi = requireStep(plan, "multi");
    require(multi.typeId == "talas.multisample", "multisample step should retain node type");
    requireNear(requireParameter(multi, "frequency").value, 0.0, 1.0e-12, "multisample frequency should compile");
    requireNear(requireParameter(multi, "gate").value, 1.0, 1.0e-12, "multisample gate should compile");
    requireTextParameter(multi, "samples", "Samples/Hit Samples/Alt");
    requireNear(requireParameter(multi, "index").value, 1.0, 1.0e-12, "multisample index should compile");
    requireBooleanParameter(multi, "loop", false);
    requireNear(requireParameter(multi, "loopStart").value, 0.0, 1.0e-12, "multisample loopStart should compile");
    requireNear(requireParameter(multi, "loopEnd").value, 1.0, 1.0e-12, "multisample loopEnd should compile");
    requireNear(requireParameter(multi, "attackTime").value, 0.0, 1.0e-12, "multisample attackTime should compile");
    requireNear(requireParameter(multi, "releaseTime").value, 0.0, 1.0e-12, "multisample releaseTime should compile");
    requireNear(requireParameter(multi, "scale").value, 0.5, 1.0e-12, "multisample scale should compile");
    require(
        multi.parameters.size() == 13
            && multi.parameters[0].id == "frequency"
            && multi.parameters[1].id == "gate"
            && multi.parameters[2].id == "samples"
            && multi.parameters[3].id == "index"
            && multi.parameters[4].id == "loop"
            && multi.parameters[5].id == "loopStart"
            && multi.parameters[6].id == "loopEnd"
            && multi.parameters[7].id == "attackTime"
            && multi.parameters[8].id == "releaseTime"
            && multi.parameters[9].id == "scale"
            && multi.parameters[10].id == "headerLoop"
            && multi.parameters[10].choice == std::optional<std::string>{"loop"}
            && multi.parameters[11].id == "gateRelease"
            && multi.parameters[11].choice == std::optional<std::string>{"playsThrough"}
            && multi.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && multi.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multi.supportsReset
            && !multi.kernelKind.has_value()
            && multi.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && multi.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && multi.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !multi.preparedIsolatedComponent,
        "multisample compile plan should preserve exact C35 prepared authority");
}

void testMultisampleGraphCompilesWithNegativeFrequency()
{
    const auto result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"frequency", "-440 hz"}})),
        makeRegistry(),
        {});

    require(result.ok(), "negative-frequency multisample graph should compile");
    const auto& multi = requireStep(result.graph->plan(), "multi");
    requireNear(requireParameter(multi, "frequency").value, -440.0, 1.0e-12, "negative multisample frequency should compile");
}

void testKeyMappedMultisampleGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeKeyMappedMultisampleOutputDocument(), makeRegistry(), {});

    require(result.ok(), "key mapped multisample graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "key mapped multisample output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "keymap", "key mapped multisample output buffer should point to keymap");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "key mapped multisample output buffer should point to audioOut");

    const auto& keymap = requireStep(plan, "keymap");
    require(keymap.typeId == "talas.keyMappedMultisample", "key mapped multisample step should retain node type");
    requireNear(requireParameter(keymap, "frequency").value, 440.0, 1.0e-9, "key mapped multisample frequency should compile");
    requireNear(requireParameter(keymap, "gate").value, 1.0, 1.0e-12, "key mapped multisample gate should compile");
    requireNear(requireParameter(keymap, "velocity").value, 0.5, 1.0e-12, "key mapped multisample velocity should compile");
    requireResourceParameter(keymap, "firstSample", "Samples/Root");
    requireNear(requireParameter(keymap, "loFreq").value, 220.0, 1.0e-9, "key mapped multisample loFreq should compile");
    requireNear(requireParameter(keymap, "hiFreq").value, 880.0, 1.0e-9, "key mapped multisample hiFreq should compile");
    requireChoiceParameter(keymap, "mapping", "byPitchRange");
    requireNear(requireParameter(keymap, "attackTime").value, 0.0, 1.0e-12, "key mapped multisample attackTime should compile");
    requireNear(requireParameter(keymap, "releaseTime").value, 0.0, 1.0e-12, "key mapped multisample releaseTime should compile");
    requireNear(requireParameter(keymap, "scale").value, 0.25, 1.0e-12, "key mapped multisample scale should compile");
    requireBooleanParameter(keymap, "loop", false);
    requireBooleanParameter(keymap, "fromDisk", false);
    requireBooleanParameter(keymap, "noTransposition", true);
    require(
        keymap.parameters.size() == 17
            && keymap.parameters[16].id == "halfStepsFrom"
            && keymap.parameters[16].choice == std::optional<std::string>{"loFreq"}
            && keymap.parameters[0].id == "frequency"
            && keymap.parameters[1].id == "gate"
            && keymap.parameters[2].id == "velocity"
            && keymap.parameters[3].id == "firstSample"
            && keymap.parameters[4].id == "loFreq"
            && keymap.parameters[5].id == "hiFreq"
            && keymap.parameters[6].id == "mapping"
            && keymap.parameters[7].id == "attackTime"
            && keymap.parameters[8].id == "releaseTime"
            && keymap.parameters[9].id == "scale"
            && keymap.parameters[10].id == "loop"
            && keymap.parameters[11].id == "fromDisk"
            && keymap.parameters[12].id == "noTransposition"
            && keymap.parameters[13].id == "headerLoop"
            && keymap.parameters[13].choice == std::optional<std::string>{"loop"}
            && keymap.parameters[14].id == "samples"
            && keymap.parameters[14].choice == std::optional<std::string>{"folder"}
            && keymap.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && keymap.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && keymap.supportsReset
            && !keymap.kernelKind.has_value()
            && keymap.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && keymap.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && keymap.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0}
            && !keymap.preparedIsolatedComponent,
        "key mapped multisample compile plan should preserve exact C36 prepared authority");
}

void testAveragingLowPassFilterGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeAveragingLowPassFilterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "averaging low pass filter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "averaging low pass output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "avg", "averaging low pass output buffer should point to avg");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "averaging low pass output buffer should point to audioOut");

    const auto& filter = requireStep(plan, "avg");
    require(filter.inputs.size() == 1
            && filter.inputs[0].inputPortId == "audioIn",
        "C139 averaging low pass inputs should use canonical descriptor order");
    require(filter.parameters.size() == 3
            && filter.parameters[0].id == "cutoff"
            && filter.parameters[1].id == "wavetable"
            && filter.parameters[2].id == "scale",
        "C139 averaging low pass parameters should use canonical descriptor order");
    require(filter.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && filter.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && filter.supportsReset,
        "C139 averaging low pass compile step should publish supported reset authority");
    require(filter.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && filter.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{{0}}
            && filter.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && filter.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "C139 averaging low pass compile step should publish exact zero latency authority");
    requireNear(requireParameter(filter, "cutoff").value, 1.0, 1.0e-12, "averaging low pass cutoff should compile");
    requireResourceParameter(filter, "wavetable", "Private");
    requireNear(requireParameter(filter, "scale").value, 1.0, 1.0e-12, "averaging low pass scale should compile");

    const auto& audioIn = requireInputBinding(filter, "audioIn");
    require(audioIn.sourceNodeId == "osc", "averaging low pass audioIn should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "averaging low pass audioIn should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "averaging low pass audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "averaging low pass audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "averaging low pass audioIn buffer should point to audioOut");
}

void testDelayWithFeedbackGraphCompilesWithBoundedAuthority()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("delay", "talas.delayWithFeedback", {
        {"delay", "250 ms"}, {"feedback", "0.25"}, {"mix", "0.5"},
    }));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "delay", "audioIn"));
    document.graph.connections.push_back(connection(
        "delay", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {48'000.0, 64});
    require(result.ok(),
        "C140 Delay With Feedback graph should compile");
    const auto& plan = result.graph->plan();
    const auto& delay = requireStep(plan, "delay");
    require(delay.inputs.size() == 1
            && delay.inputs[0].inputPortId == "audioIn"
            && delay.parameters.size() == 10
            && delay.parameters[0].id == "delay"
            && delay.parameters[1].id == "feedback"
            && delay.parameters[2].id == "mix"
            && delay.parameters[3].id == "type"
            && delay.parameters[4].id == "scale"
            && delay.parameters[5].id == "delayScale"
            && delay.parameters[6].id == "interpolation"
            && delay.parameters[7].id == "wavetable"
            && delay.parameters[8].id == "prezero"
            && delay.parameters[9].id == "smoothDelayChanges"
            && delay.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && delay.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && delay.supportsReset
            && !delay.kernelKind.has_value()
            && delay.resolvedLatencyFrames
                == std::optional<std::size_t>{0}
            && delay.inputLatencyFrames
                == std::vector<std::optional<std::size_t>>{{0}}
            && delay.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && delay.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "C140 Delay With Feedback compile step should publish exact supported reset and zero-latency authority");
}

void testNoiseGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makeNoiseOutputDocument({
        {"initialState", "0.25"},
        choiceParameter("type", "white"),
        {"frequency", "440 hz"},
        {"centerValue", "0.0"},
        {"scale", "1.0"},
    });

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "noise graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "noise output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "noise", "noise output buffer should point to noise");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "noise output buffer should point to audioOut");
    require(hasBuffer(plan, "noise", "audioOut"), "noise output should have a buffer");
}

void testInputOutputCharacteristicGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makeInputOutputCharacteristicDocument({
        textParameter("inValues", "-1 0 1"),
        textParameter("outValues", "1 0 1"),
        {"smoothing", "0.5"},
        {"startSlope", "0"},
        {"endSlope", "0"},
    });

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "input/output characteristic graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "input/output characteristic output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mapper", "input/output characteristic output buffer should point to mapper");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "input/output characteristic output buffer should point to audioOut");

    const auto& mapper = requireStep(plan, "mapper");
    const auto& audioIn = requireInputBinding(mapper, "audioIn");
    require(audioIn.sourceNodeId == "osc", "input/output characteristic input should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "input/output characteristic input should bind to oscillator audio output");
}

void testWaveshaperGraphCompilesWithPolynomialOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeWaveshaperOutputDocument(), makeRegistry(), {});

    require(result.ok(), "waveshaper polynomial graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "waveshaper output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "shape", "waveshaper output buffer should point to shape");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "waveshaper output buffer should point to audioOut");

    const auto& shape = requireStep(plan, "shape");
    requireChoiceParameter(shape, "shapeFrom", "polynomial");
    requireTextParameter(shape, "coefficients", "0 1");
    requireBooleanParameter(shape, "fromMemoryWriter", false);

    const auto& audioIn = requireInputBinding(shape, "audioIn");
    require(audioIn.sourceNodeId == "osc", "waveshaper input should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "waveshaper input should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "waveshaper audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "waveshaper audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "waveshaper audioIn buffer should point to audioOut");
}

void testMultiplyingWaveshaperGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeMultiplyingWaveshaperOutputDocument(), makeRegistry(), {});

    require(result.ok(), "multiplying waveshaper graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "multiplying waveshaper output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "shape", "multiplying waveshaper output buffer should point to shape");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "multiplying waveshaper output buffer should point to audioOut");

    const auto& shape = requireStep(plan, "shape");
    requireNear(requireParameter(shape, "scale").value, 0.5, 1.0e-12, "multiplying waveshaper scale should compile");
    requireResourceParameter(shape, "wavetable", "Tables/Dynamics");

    const auto& nonlinearInput = requireInputBinding(shape, "nonlinearInput");
    require(nonlinearInput.sourceNodeId == "index", "multiplying waveshaper nonlinear input should bind to index oscillator");
    require(nonlinearInput.sourcePortId == "audioOut", "multiplying waveshaper nonlinear input should bind oscillator audio output");
    require(nonlinearInput.bufferIndex < plan.buffers.size(), "multiplying waveshaper nonlinear buffer index should be valid");
    require(plan.buffers[nonlinearInput.bufferIndex].nodeId == "index", "multiplying waveshaper nonlinear buffer should point to index");
    require(plan.buffers[nonlinearInput.bufferIndex].portId == "audioOut", "multiplying waveshaper nonlinear buffer should point to audioOut");

    const auto& audioIn = requireInputBinding(shape, "audioIn");
    require(audioIn.sourceNodeId == "input", "multiplying waveshaper audio input should bind to input oscillator");
    require(audioIn.sourcePortId == "audioOut", "multiplying waveshaper audio input should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "multiplying waveshaper audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "input", "multiplying waveshaper audioIn buffer should point to input");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "multiplying waveshaper audioIn buffer should point to audioOut");
}

void testIteratedWaveshaperGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeIteratedWaveshaperOutputDocument(), makeRegistry(), {});

    require(result.ok(), "iterated waveshaper graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "iterated waveshaper output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "shape", "iterated waveshaper output buffer should point to shape");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "iterated waveshaper output buffer should point to audioOut");

    const auto& shape = requireStep(plan, "shape");
    requireNear(requireParameter(shape, "iterations").value, 2.0, 1.0e-12, "iterated waveshaper iterations should compile");
    requireNear(requireParameter(shape, "scale").value, 0.5, 1.0e-12, "iterated waveshaper scale should compile");
    requireResourceParameter(shape, "wavetable", "Tables/Dynamics");

    const auto& audioIn = requireInputBinding(shape, "audioIn");
    require(audioIn.sourceNodeId == "osc", "iterated waveshaper audio input should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "iterated waveshaper audio input should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "iterated waveshaper audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "iterated waveshaper audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "iterated waveshaper audioIn buffer should point to audioOut");
}

void testSampleAndHoldGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeSampleAndHoldOutputDocument({{"holdTime", "0.5 s"}, {"offsetTime", "0 s"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "sample and hold graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "hold", "value"), "sample and hold output should have a buffer");

    const auto& hold = requireStep(plan, "hold");
    const auto& value = requireInputBinding(hold, "value");
    require(value.sourceNodeId == "ramp", "sample and hold value input should bind to ramp");
    require(value.sourcePortId == "value", "sample and hold value input should bind to ramp value output");
    require(value.bufferIndex < plan.buffers.size(), "sample and hold value buffer index should be valid");
    require(plan.buffers[value.bufferIndex].nodeId == "ramp", "sample and hold value buffer should point to ramp");
    require(plan.buffers[value.bufferIndex].portId == "value", "sample and hold value buffer should point to ramp value");
}

void testTalasRampGraphCompilesWithHotControlBindings()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("vca", "talas.vca"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.nodes.push_back(node("trigger", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node("duration", "talas.constant", {{"value", "0.5"}}));
    document.graph.nodes.push_back(node(
        "ramp",
        "talas.ramp",
        {{"trigger", "0"}, {"duration", "1 s"}, choiceParameter("variant", "repeatingFullRamp")}));
    document.graph.connections.push_back(connection("osc", "audioOut", "vca", "audioIn"));
    document.graph.connections.push_back(connection("trigger", "value", "ramp", "trigger"));
    document.graph.connections.push_back(connection("duration", "value", "ramp", "duration"));
    document.graph.connections.push_back(connection("ramp", "value", "vca", "gain"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "Ramp graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "ramp", "value"), "Ramp output should have a control buffer");
    const auto& ramp = requireStep(plan, "ramp");
    require(ramp.typeId == "talas.ramp", "Ramp compile step should retain its node identity");
    requireChoiceParameter(ramp, "variant", "repeatingFullRamp");
    require(requireInputBinding(ramp, "trigger").sourceNodeId == "trigger", "Ramp trigger should bind its control source");
    require(requireInputBinding(ramp, "duration").sourceNodeId == "duration", "Ramp duration should bind its control source");
}

void testPulseTrainGraphCompilesWithControlOutputBuffer()
{
    const auto document = makePulseTrainOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "pulse train graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "pulse", "value"), "pulse train output should have a buffer");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "pulse", "vca gain input should bind to pulse train");
    require(gain.sourcePortId == "value", "vca gain input should bind to pulse train value output");
    require(gain.bufferIndex < plan.buffers.size(), "pulse train value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "pulse", "pulse train value buffer should point to pulse node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "pulse train value buffer should point to pulse value");
    const auto& pulse = requireStep(plan, "pulse");
    require(
        pulse.offlineSupport == talas::core::ExecutionSupport::Supported
            && pulse.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && pulse.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.supportsReset
            && pulse.resolvedLatencyFrames == 0
            && !pulse.kernelKind.has_value()
            && !pulse.preparedIsolatedComponent,
        "C183 Pulse Train compile step should retain exact promoted authority");
}

void testPulseTrainIgnoresDutyCycleWhenVariableDutyCycleIsDisabled()
{
    const auto result = talas::runtime::compileProject(
        makePulseTrainOutputDocument({{"period", "1 s"}, booleanParameter("variableDutyCycle", false), {"dutyCycle", "1.5"}, {"gate", "1"}}),
        makeRegistry(),
        {});

    require(result.ok(), "pulse train compile should ignore dutyCycle when variableDutyCycle is disabled");
}

void testPulseGeneratorGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makePulseGeneratorOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "pulse generator graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "pulse generator output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "pulse", "pulse generator output buffer should point to pulse node");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "pulse generator output buffer should point to audioOut");

    const auto& pulse = requireStep(plan, "pulse");
    requireNear(requireParameter(pulse, "frequency").value, 1.0, 1.0e-12, "pulse generator frequency should compile");
    requireTextParameter(pulse, "modulation", "");
    requireNear(requireParameter(pulse, "maxMI").value, 0.0, 1.0e-12, "pulse generator maxMI should compile");
    requireChoiceParameter(pulse, "interpolation", "linear");
    requireNear(requireParameter(pulse, "envelope").value, 1.0, 1.0e-12, "pulse generator envelope should compile");
    requireNear(requireParameter(pulse, "dutyCycle").value, 0.25, 1.0e-12, "pulse generator dutyCycle should compile");
    requireNear(requireParameter(pulse, "reset").value, 0.0, 1.0e-12, "pulse generator reset should compile");
    require(
        pulse.offlineSupport == talas::core::ExecutionSupport::Subset
            && pulse.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && pulse.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.supportsReset
            && pulse.resolvedLatencyFrames == 0
            && !pulse.kernelKind.has_value()
            && !pulse.preparedIsolatedComponent,
        "C94 Pulse Generator compile step should retain exact state/reset authority");
}

void testMultislopeFunctionGeneratorGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeMultislopeFunctionGeneratorOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "multislope function generator graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "multi", "value"), "multislope function generator output should have a buffer");

    const auto& multi = requireStep(plan, "multi");
    requireTextParameter(multi, "timePoints", "{0 s} {1 s} {2 s}");
    requireTextParameter(multi, "slopes", "{-1} {1}");
    requireNear(requireParameter(multi, "level").value, 1.0, 1.0e-12, "multislope level should compile");
    requireNear(requireParameter(multi, "gate").value, 1.0, 1.0e-12, "multislope gate should compile");
    requireNear(requireParameter(multi, "rate").value, 1.0, 1.0e-12, "multislope rate should compile");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "multi", "vca gain input should bind to multislope function generator");
    require(gain.sourcePortId == "value", "vca gain input should bind to multislope function generator value output");
    require(gain.bufferIndex < plan.buffers.size(), "multislope value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "multi", "multislope value buffer should point to multi node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "multislope value buffer should point to multi value");
}

void testMultisegmentEnvelopeGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeMultisegmentEnvelopeOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "multisegment envelope graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "env", "value"), "multisegment envelope output should have a buffer");

    const auto& env = requireStep(plan, "env");
    requireTextParameter(env, "durations", "{1 s} {1 s}");
    requireTextParameter(env, "breakPoints", "{0} {1} {0.5}");
    requireNear(requireParameter(env, "startLoop").value, 0.0, 1.0e-12, "multisegment startLoop should compile");
    requireNear(requireParameter(env, "endLoop").value, 0.0, 1.0e-12, "multisegment endLoop should compile");
    requireNear(requireParameter(env, "level").value, 1.0, 1.0e-12, "multisegment level should compile");
    requireNear(requireParameter(env, "gate").value, 1.0, 1.0e-12, "multisegment gate should compile");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "env", "vca gain input should bind to multisegment envelope");
    require(gain.sourcePortId == "value", "vca gain input should bind to multisegment envelope value output");
    require(gain.bufferIndex < plan.buffers.size(), "multisegment value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "env", "multisegment value buffer should point to envelope node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "multisegment value buffer should point to envelope value");
}

void testGraphicalEnvelopeGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeGraphicalEnvelopeOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "graphical envelope graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "env", "value"), "graphical envelope output should have a buffer");

    const auto& env = requireStep(plan, "env");
    requireTextParameter(env, "envelope", "{0 s} {0} {1 s} {1} {2 s} {0.5}");
    requireNear(requireParameter(env, "level").value, 1.0, 1.0e-12, "graphical envelope level should compile");
    requireNear(requireParameter(env, "gate").value, 1.0, 1.0e-12, "graphical envelope gate should compile");
    requireNear(requireParameter(env, "rate").value, 1.0, 1.0e-12, "graphical envelope rate should compile");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "env", "vca gain input should bind to graphical envelope");
    require(gain.sourcePortId == "value", "vca gain input should bind to graphical envelope value output");
    require(gain.bufferIndex < plan.buffers.size(), "graphical envelope value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "env", "graphical envelope value buffer should point to envelope node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "graphical envelope value buffer should point to envelope value");
}

void testInvalidGraphicalEnvelopeParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeGraphicalEnvelopeOutputDocument(graphicalEnvelopeParameters({{"level", "-0.1"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "env", "level");

    result = talas::runtime::compileProject(makeGraphicalEnvelopeOutputDocument(graphicalEnvelopeParameters({{"level", "1.1"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "env", "level");

    result = talas::runtime::compileProject(makeGraphicalEnvelopeOutputDocument(graphicalEnvelopeParameters({{"gate", "1 / 0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "env", "gate");

    result = talas::runtime::compileProject(makeGraphicalEnvelopeOutputDocument(graphicalEnvelopeParameters({{"rate", "0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "env", "rate");
}

void testFunctionGeneratorGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeFunctionGeneratorOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "function generator graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "fg", "value"), "function generator output should have a buffer");

    const auto& fg = requireStep(plan, "fg");
    requireNear(requireParameter(fg, "trigger").value, 1.0, 1.0e-12, "function generator trigger should compile");
    requireNear(requireParameter(fg, "onDuration").value, 1.0, 1.0e-12, "function generator onDuration should compile");
    requireResourceParameter(fg, "wavetable", "Tables/Envelope");
    requireBooleanParameter(fg, "fromMemoryWriter", false);

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "fg", "vca gain input should bind to function generator");
    require(gain.sourcePortId == "value", "vca gain input should bind to function generator value output");
    require(gain.bufferIndex < plan.buffers.size(), "function generator value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "fg", "function generator value buffer should point to fg node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "function generator value buffer should point to fg value");
}

void testChopperGraphCompilesWithAudioOutputBuffer()
{
    const auto document = makeChopperOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "chopper graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "chopper output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "chop", "chopper output buffer should point to chopper");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "chopper output buffer should point to audioOut");

    const auto& chop = requireStep(plan, "chop");
    requireNear(requireParameter(chop, "grainDuration").value, 1.0, 1.0e-12, "chopper grainDuration should compile");
    requireNear(requireParameter(chop, "interGrainDelay").value, 0.5, 1.0e-12, "chopper interGrainDelay should compile");
    requireResourceParameter(chop, "wavetable", "Tables/Grain");

    const auto& audioIn = requireInputBinding(chop, "audioIn");
    require(audioIn.sourceNodeId == "osc", "chopper input should bind to oscillator");
    require(audioIn.sourcePortId == "audioOut", "chopper input should bind oscillator audio output");
    require(audioIn.bufferIndex < plan.buffers.size(), "chopper audioIn buffer index should be valid");
    require(plan.buffers[audioIn.bufferIndex].nodeId == "osc", "chopper audioIn buffer should point to oscillator");
    require(plan.buffers[audioIn.bufferIndex].portId == "audioOut", "chopper audioIn buffer should point to audioOut");
}

void testArGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeArOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "ar graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "env", "value"), "ar output should have a buffer");

    const auto& env = requireStep(plan, "env");
    require(
        env.offlineSupport == talas::core::ExecutionSupport::Supported
            && env.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && env.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && env.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && env.supportsReset
            && env.resolvedLatencyFrames == 0
            && env.cumulativeOutputLatencyFrames == 0
            && !env.kernelKind.has_value()
            && !env.preparedIsolatedComponent
            && env.eventValueScopeId == env.scopeId,
        "C163 compile plan should preserve promoted AR authority");
    requireNear(requireParameter(env, "attackTime").value, 0.5, 1.0e-12, "ar attackTime should compile");
    requireNear(requireParameter(env, "releaseTime").value, 0.25, 1.0e-12, "ar releaseTime should compile");
    requireChoiceParameter(env, "type", "linear");
    requireNear(requireParameter(env, "scale").value, 1.0, 1.0e-12, "ar scale should compile");
    requireNear(requireParameter(env, "gate").value, 1.0, 1.0e-12, "ar gate should compile");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "env", "vca gain input should bind to ar envelope");
    require(gain.sourcePortId == "value", "vca gain input should bind to ar value output");
    require(gain.bufferIndex < plan.buffers.size(), "ar value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "env", "ar value buffer should point to envelope node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "ar value buffer should point to envelope value");
}

void testAdsrGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeAdsrOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "adsr graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "env", "value"), "adsr output should have a buffer");

    const auto& env = requireStep(plan, "env");
    requireNear(requireParameter(env, "attackTime").value, 0.5, 1.0e-12, "adsr attackTime should compile");
    requireNear(requireParameter(env, "decayTime").value, 0.5, 1.0e-12, "adsr decayTime should compile");
    requireNear(requireParameter(env, "sustainLevel").value, 0.25, 1.0e-12, "adsr sustainLevel should compile");
    requireNear(requireParameter(env, "releaseTime").value, 0.25, 1.0e-12, "adsr releaseTime should compile");
    requireChoiceParameter(env, "type", "linear");
    requireNear(requireParameter(env, "scale").value, 1.0, 1.0e-12, "adsr scale should compile");
    requireNear(requireParameter(env, "gate").value, 1.0, 1.0e-12, "adsr gate should compile");
    requireNear(requireParameter(env, "legato").value, 0.0, 1.0e-12, "adsr legato should compile");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "env", "vca gain input should bind to adsr envelope");
    require(gain.sourcePortId == "value", "vca gain input should bind to adsr value output");
    require(gain.bufferIndex < plan.buffers.size(), "adsr value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "env", "adsr value buffer should point to envelope node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "adsr value buffer should point to envelope value");
}

void testTriggeredSampleAndHoldGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeTriggeredSampleAndHoldOutputDocument({{"trigger", "1.0"}});

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "triggered sample and hold graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "hold", "value"), "triggered sample and hold output should have a buffer");

    const auto& hold = requireStep(plan, "hold");
    const auto& value = requireInputBinding(hold, "value");
    require(value.sourceNodeId == "ramp", "triggered sample and hold value input should bind to ramp");
    require(value.sourcePortId == "value", "triggered sample and hold value input should bind to ramp value output");
    require(value.bufferIndex < plan.buffers.size(), "triggered sample and hold value buffer index should be valid");
    require(plan.buffers[value.bufferIndex].nodeId == "ramp", "triggered sample and hold value buffer should point to ramp");
    require(plan.buffers[value.bufferIndex].portId == "value", "triggered sample and hold value buffer should point to ramp value");
}

void testTriggeredTableReadGraphCompilesWithControlOutputBuffer()
{
    const auto document = makeTriggeredTableReadOutputDocument();

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "triggered table read graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "table", "value"), "triggered table read output should have a buffer");

    const auto& table = requireStep(plan, "table");
    requireNear(requireParameter(table, "trigger").value, 1.0, 1.0e-12, "triggered table read trigger should compile");
    requireResourceParameter(table, "wavetable", "Tables/Steps");
    requireNear(requireParameter(table, "gate").value, 1.0, 1.0e-12, "triggered table read gate should compile");
    requireBooleanParameter(table, "ignoreLoops", true);
    requireBooleanParameter(table, "fromMemoryWriter", false);

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "table", "vca gain input should bind to triggered table read");
    require(gain.sourcePortId == "value", "vca gain input should bind to triggered table read value output");
    require(gain.bufferIndex < plan.buffers.size(), "triggered table read value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "table", "triggered table read value buffer should point to table node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "triggered table read value buffer should point to table value");
}

void testFrequencyTrackerGraphCompilesWithControlOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeFrequencyTrackerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "frequency tracker graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "tracker", "value"), "frequency tracker output should have a buffer");

    const auto& tracker = requireStep(plan, "tracker");
    require(tracker.typeId == "talas.frequencyTracker", "frequency tracker step should keep type id");
    requireNear(requireParameter(tracker, "minFrequency").value, 1.0, 1.0e-12, "frequency tracker minFrequency should compile");
    requireNear(requireParameter(tracker, "maxFrequency").value, 8.0, 1.0e-12, "frequency tracker maxFrequency should compile");
    requireNear(requireParameter(tracker, "confidence").value, 0.5, 1.0e-12, "frequency tracker confidence should compile");
    requireNear(requireParameter(tracker, "scale").value, 0.25, 1.0e-12, "frequency tracker scale should compile");
    requireNear(requireParameter(tracker, "detectors").value, 10.0, 1.0e-12, "frequency tracker detectors should compile");
    requireNear(requireParameter(tracker, "emphasis").value, 0.0, 1.0e-12, "frequency tracker emphasis should compile");

    const auto& audioIn = requireInputBinding(tracker, "audioIn");
    require(audioIn.sourceNodeId == "source", "frequency tracker audioIn should bind source oscillator");
    require(audioIn.sourcePortId == "audioOut", "frequency tracker audioIn should bind source oscillator audio output");

    const auto& vca = requireStep(plan, "vca");
    const auto& gain = requireInputBinding(vca, "gain");
    require(gain.sourceNodeId == "tracker", "vca gain should bind frequency tracker");
    require(gain.sourcePortId == "value", "vca gain should bind frequency tracker value");
    require(gain.bufferIndex < plan.buffers.size(), "frequency tracker value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "tracker", "frequency tracker value buffer should point to tracker node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "frequency tracker value buffer should point to value");
}

void testTimeGateGraphsCompileWithAudioOutputBuffers()
{
    for (const auto* typeId : {"talas.timeStopper", "talas.waitUntil"})
    {
        const auto result = talas::runtime::compileProject(
            makeTimeGateOutputDocument(typeId, {{"resume", "1.0"}, booleanParameter("resumeOnZero", false)}),
            makeRegistry(),
            {});

        require(result.ok(), std::string(typeId) + " graph should compile");
        const auto& plan = result.graph->plan();
        require(plan.outputBufferIndex < plan.buffers.size(), std::string(typeId) + " output buffer index should be valid");
        require(plan.buffers[plan.outputBufferIndex].nodeId == "gate", std::string(typeId) + " output buffer should point to gate");
        require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", std::string(typeId) + " output buffer should point to audioOut");

        const auto& gate = requireStep(plan, "gate");
        const auto& audioIn = requireInputBinding(gate, "audioIn");
        require(audioIn.sourceNodeId == "osc", std::string(typeId) + ".audioIn should bind to oscillator");
        require(audioIn.sourcePortId == "audioOut", std::string(typeId) + ".audioIn should bind to oscillator audio output");
    }
}

void testTimeControlGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeTimeControlOutputDocument(), makeRegistry(), {});

    require(result.ok(), "time control graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "time control output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "time", "time control output buffer should point to time");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "time control output buffer should point to audioOut");

    const auto& time = requireStep(plan, "time");
    require(time.typeId == "talas.timeControl", "time control step should keep type id");
    requireNear(requireParameter(time, "rate").value, 0.5, 1.0e-12, "time control rate should compile");
    require(requireInputBinding(time, "audioIn").sourceNodeId == "osc", "time control audioIn should bind oscillator");
}

void testTimeOffsetGraphCompilesWithRetrogradeEnabled()
{
    const auto result = talas::runtime::compileProject(
        makeTimeOffsetOutputDocument({
            {"offset", "0.25 s"},
            booleanParameter("reverse", false),
            booleanParameter("retrograde", true),
        }),
        makeRegistry(),
        {});

    require(result.ok(), "time offset should compile with retrograde enabled;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "time offset output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "offset", "time offset output buffer should point to offset");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "time offset output buffer should point to audioOut");

    const auto& offset = requireStep(plan, "offset");
    require(offset.typeId == "talas.timeOffset", "time offset step should keep type id");
    require(
        offset.offlineSupport == talas::core::ExecutionSupport::Supported
            && offset.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && offset.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && offset.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && offset.supportsReset
            && !offset.kernelKind.has_value()
            && !offset.preparedIsolatedComponent
            && offset.resolvedLatencyFrames == std::optional<std::size_t>{0}
            && offset.cumulativeInputLatencyFrames
                == std::optional<std::size_t>{0}
            && offset.cumulativeOutputLatencyFrames
                == std::optional<std::size_t>{0},
        "C142 time offset compile step should preserve its exact Supported authority, its timing not latency");
    requireNear(requireParameter(offset, "offset").value, 0.25, 1.0e-12, "time offset offset should compile");
    requireBooleanParameter(offset, "reverse", false);
    requireBooleanParameter(offset, "retrograde", true);
    require(requireInputBinding(offset, "audioIn").sourceNodeId == "osc", "time offset audioIn should bind oscillator");
}

void testWarpedTimeIndexGraphCompilesWithControlOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeWarpedTimeIndexOutputDocument(), makeRegistry(), {});

    require(result.ok(), "warped time index graph should compile");
    const auto& plan = result.graph->plan();
    require(hasBuffer(plan, "index", "value"), "warped time index output should have a buffer");

    const auto& index = requireStep(plan, "index");
    require(
        index.typeId == "talas.warpedTimeIndex"
            && index.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && index.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && index.supportsReset
            && !index.kernelKind.has_value()
            && !index.preparedIsolatedComponent
            && index.scopeId == index.eventValueScopeId
            && index.inputs.empty()
            && index.parameters.size() == 6,
        "warped time index compile step should preserve the exact C147 promoted authority");
    constexpr std::array<std::string_view, 6> parameterOrder{
        "idealTimePoints0", "idealTimePoints1", "currentTimePoints",
        "trigger", "rate", "morph"};
    for (std::size_t parameter = 0;
         parameter < parameterOrder.size();
         ++parameter)
    {
        require(
            index.parameters[parameter].id == parameterOrder[parameter],
            "warped time index compile step should preserve exact parameter order");
    }
    require(
        index.resolvedLatencyFrames == 0
            && index.cumulativeInputLatencyFrames == 0
            && index.cumulativeOutputLatencyFrames == 0,
        "warped time index compile step should preserve exact zero-latency ancestry");
    requireTextParameter(index, "idealTimePoints0", "{0 s} {2 s}");
    requireTextParameter(index, "idealTimePoints1", "{0 s} {1 s}");
    requireTextParameter(index, "currentTimePoints", "{0 s} {4 s}");
    requireNear(requireParameter(index, "trigger").value, 1.0, 1.0e-12, "warped time index trigger should compile");
    requireNear(requireParameter(index, "rate").value, 1.0, 1.0e-12, "warped time index rate should compile");
    requireNear(requireParameter(index, "morph").value, 0.0, 1.0e-12, "warped time index morph should compile");

    const auto& gain = requireInputBinding(requireStep(plan, "vca"), "gain");
    require(gain.sourceNodeId == "index", "vca gain input should bind to warped time index");
    require(gain.sourcePortId == "value", "vca gain input should bind to warped time index value output");
    require(gain.bufferIndex < plan.buffers.size(), "warped time index value buffer index should be valid");
    require(plan.buffers[gain.bufferIndex].nodeId == "index", "warped time index value buffer should point to index node");
    require(plan.buffers[gain.bufferIndex].portId == "value", "warped time index value buffer should point to value");
}

void testFrequencyScaleGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeFrequencyScaleOutputDocument(), makeRegistry(), {});

    require(result.ok(), "frequency scale graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "frequency scale output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "scale", "frequency scale output buffer should point to scale");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "frequency scale output buffer should point to audioOut");

    const auto& scale = requireStep(plan, "scale");
    require(scale.typeId == "talas.frequencyScale", "frequency scale step should keep type id");
    requireNear(requireParameter(scale, "frequencyScale").value, 1.0, 1.0e-12, "frequency scale should compile");
    requireNear(requireParameter(scale, "maxScale").value, 1.0, 1.0e-12, "frequency scale maxScale should compile");
    requireResourceParameter(scale, "window", "Private");
    requireNear(requireParameter(scale, "delay").value, 0.0, 1.0e-12, "frequency scale delay should compile");
    require(scale.resolvedLatencyFrames == 0
            && scale.supportsReset,
            "neutral static FrequencyScale should publish exact delay latency and reset authority");

    const auto& audioIn = requireInputBinding(scale, "audioIn");
    require(audioIn.sourceNodeId == "osc", "frequency scale audioIn should bind oscillator");
    require(audioIn.sourcePortId == "audioOut", "frequency scale audioIn should bind oscillator audio output");

    const auto& freqTracker = requireInputBinding(scale, "freqTracker");
    require(freqTracker.sourceNodeId == "tracker", "frequency scale freqTracker should bind tracker");
    require(freqTracker.sourcePortId == "value", "frequency scale freqTracker should bind tracker value");

    const auto active = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({
            {"frequencyScale", "1.5"},
            {"maxScale", "2.0"},
            resourceParameter("window", "Private"),
            {"delay", "0.001"},
        }),
        makeRegistry(),
        {});
    require(active.ok(), "active FrequencyScale graph should compile");
    const auto& activeScale = requireStep(active.graph->plan(), "scale");
    require(activeScale.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames + 48
            && activeScale.supportsReset,
            "active FrequencyScale should publish delay plus exact phase-vocoder latency");

    // Grains are late by the Delay alone, and play ratios a phase
    // vocoder cannot.
    const auto grains = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({
            {"frequencyScale", "3"},
            {"maxScale", "4"},
            resourceParameter("window", "Private"),
            {"delay", "0.001"},
            choiceParameter("scaling", "grains"),
        }),
        makeRegistry(),
        {});
    require(grains.ok(), "a granulating FrequencyScale of 3 should compile");
    const auto& grainScale = requireStep(grains.graph->plan(), "scale");
    require(grainScale.resolvedLatencyFrames == std::optional<std::size_t>{48} && grainScale.supportsReset,
        "a granulating FrequencyScale should publish its Delay alone as its latency");

    auto connectedDelay = makeFrequencyScaleOutputDocument({
        {"frequencyScale", "1.5"},
        {"maxScale", "2.0"},
        resourceParameter("window", "Private"),
        {"delay", "0"},
    });
    connectedDelay.graph.nodes.push_back(
        node("delay", "talas.constant", {{"value", "0"}}));
    connectedDelay.graph.connections.push_back(
        connection("delay", "value", "scale", "delay"));
    const auto rejected = talas::runtime::compileProject(
        connectedDelay, makeRegistry(), {});
    requireCompileFailure(
        rejected,
        DiagnosticCode::GraphValidationFailed,
        "scale",
        "frequencyScale");
}

void testTimeFrequencyScaleGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeTimeFrequencyScaleOutputDocument(), makeRegistry(), {});

    require(result.ok(), "time frequency scale graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "time frequency scale output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "tfs", "time frequency scale output buffer should point to tfs");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "time frequency scale output buffer should point to audioOut");

    const auto& tfs = requireStep(plan, "tfs");
    require(tfs.typeId == "talas.timeFrequencyScale", "time frequency scale step should keep type id");
    require(tfs.inputs.empty(), "time frequency scale should not require inputs");
    requireNear(requireParameter(tfs, "frequencyScale").value, 0.5, 1.0e-12, "time frequency scale frequencyScale should compile");
    requireNear(requireParameter(tfs, "rate").value, 0.5, 1.0e-12, "time frequency scale rate should compile");
    requireNear(requireParameter(tfs, "gate").value, 1.0, 1.0e-12, "time frequency scale gate should compile");
    requireNear(requireParameter(tfs, "minInputFreq").value, 20.0, 1.0e-12, "time frequency scale minInputFreq should compile");
    requireNear(requireParameter(tfs, "maxInputFreq").value, 2000.0, 1.0e-12, "time frequency scale maxInputFreq should compile");
    requireNear(requireParameter(tfs, "maxFreqScale").value, 1.0, 1.0e-12, "time frequency scale maxFreqScale should compile");
    requireNear(requireParameter(tfs, "detectors").value, 10.0, 1.0e-12, "time frequency scale detectors should compile");
    requireBooleanParameter(tfs, "fromDisk", false);
    requireResourceParameter(tfs, "sample", "Samples/TFS");
    requireBooleanParameter(tfs, "fromMemoryWriter", false);
    require(tfs.resolvedLatencyFrames == 0 && tfs.supportsReset,
            "same-ratio TimeFrequencyScale should expose zero latency and reset authority");

    const auto corrected = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(
            timeFrequencyScaleParameters({
                {"frequencyScale", "1.0"},
                {"rate", "0.5"},
            })),
        makeRegistry(),
        {});
    require(corrected.ok(),
            "bounded corrective TimeFrequencyScale should compile");
    require(requireStep(corrected.graph->plan(), "tfs").resolvedLatencyFrames
                == talas::dsp::timeFrequencyScaleLatencyFrames(talas::dsp::timeFrequencyScaleAnalysisFrameSize(
                    corrected.graph->renderSettings().sampleRate, 20.0)),
            "corrective TimeFrequencyScale should publish its source analysis latency");
}

void testTimeFrequencyScaleGraphCompilesWithFastRate()
{
    const auto result = talas::runtime::compileProject(
        // A Rate above 1 compresses time only as projects before format 92
        // did (later projects keep it at most 1).
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"frequencyScale", "2.0"}, {"rate", "2.0"}, {"maxFreqScale", "2.0"},
            choiceParameter("rateAboveOne", "legacyCompresses")})),
        makeRegistry(),
        {});

    require(result.ok(), "fast-rate time frequency scale graph should compile");
    const auto& tfs = requireStep(result.graph->plan(), "tfs");
    requireNear(requireParameter(tfs, "rate").value, 2.0, 1.0e-12, "fast time frequency scale rate should compile");
}

void testMonotonizerGraphCompilesAsNeutralPitchIdentity()
{
    const auto result = talas::runtime::compileProject(makeMonotonizerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "neutral monotonizer graph should compile;" + compileDiagnosticsSummary(result));
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "monotonizer output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mono", "monotonizer output buffer should point to mono");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "monotonizer output buffer should point to audioOut");

    const auto& mono = requireStep(plan, "mono");
    require(mono.typeId == "talas.monotonizer", "monotonizer step should keep type id");
    requireNear(requireParameter(mono, "frequency").value, 440.0, 1.0e-12, "monotonizer frequency should compile");
    requireNear(requireParameter(mono, "minInputPitch").value, 440.0, 1.0e-12, "monotonizer minInputPitch should compile");
    requireNear(requireParameter(mono, "maxInputPitch").value, 440.0, 1.0e-12, "monotonizer maxInputPitch should compile");
    require(requireInputBinding(mono, "audioIn").sourceNodeId == "osc", "monotonizer audioIn should bind oscillator");
    require(mono.resolvedLatencyFrames == 0 && mono.supportsReset,
            "static neutral Monotonizer should expose zero latency and reset authority");

    const auto active = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({
            {"frequency", "660 hz"},
            {"minInputPitch", "440 hz"},
            {"maxInputPitch", "440 hz"},
        }),
        makeRegistry(),
        {});
    require(active.ok(), "known-pitch Monotonizer retarget should compile");
    const auto& activeMono = requireStep(active.graph->plan(), "mono");
    require(activeMono.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames,
            "active Monotonizer should publish exact phase-vocoder latency");
}

void testConnectedMonotonizerFallbacksAreInertAtCompile()
{
    auto document = makeMonotonizerOutputDocument({
        {"frequency", "1 / 0"},
        {"minInputPitch", "1 / 0"},
        {"maxInputPitch", "1 / 0"},
    });
    for (const auto [nodeId, value, portId] : {
             std::tuple{"target", "150", "frequency"},
             std::tuple{"minimum", "100", "minInputPitch"},
             std::tuple{"maximum", "100", "maxInputPitch"},
         })
    {
        document.graph.nodes.push_back(node(
            nodeId, "talas.constant", {{"value", value}}));
        document.graph.connections.push_back(connection(
            nodeId, "value", "mono", portId));
    }

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {48000.0, 256});
    require(result.ok(),
        "C105 connected Monotonizer controls should exclusively own selection even when every hidden fallback is invalid: "
            + compileDiagnosticsSummary(result));
    const auto& mono = requireStep(result.graph->plan(), "mono");
    require(mono.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames,
        "C105 connected Monotonizer authority should select the active latency profile without deriving a hidden fallback ratio");
}

void testSimplePitchShifterGraphCompilesWithAudioOutputBuffer()
{
    const auto result = talas::runtime::compileProject(makeSimplePitchShifterOutputDocument(), makeRegistry(), {});

    require(result.ok(), "simple pitch shifter graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "simple pitch shifter output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "pitch", "simple pitch shifter output buffer should point to pitch");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "simple pitch shifter output buffer should point to audioOut");

    const auto& pitch = requireStep(plan, "pitch");
    require(pitch.typeId == "talas.simplePitchShifter", "simple pitch shifter step should keep type id");
    requireNear(requireParameter(pitch, "interval").value, 0.0, 1.0e-12, "simple pitch shifter interval should compile");
    requireNear(requireParameter(pitch, "minInputPitch").value, 20.0, 1.0e-12, "simple pitch shifter minInputPitch should compile");
    requireNear(requireParameter(pitch, "maxInputPitch").value, 2000.0, 1.0e-12, "simple pitch shifter maxInputPitch should compile");
    require(requireInputBinding(pitch, "audioIn").sourceNodeId == "osc", "simple pitch shifter audioIn should bind oscillator");
    require(pitch.resolvedLatencyFrames == 0
                && pitch.cumulativeOutputLatencyFrames == 0,
            "neutral simple pitch shifter should compile with zero latency");

    const auto shifted = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({
            {"interval", "12"},
            {"minInputPitch", "20 hz"},
            {"maxInputPitch", "2000 hz"},
        }),
        makeRegistry(),
        {});
    require(shifted.ok(),
            "non-neutral simple pitch shifter graph should compile");
    const auto& shiftedPitch = requireStep(shifted.graph->plan(), "pitch");
    require(shiftedPitch.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && shiftedPitch.cumulativeInputLatencyFrames == 0
            && shiftedPitch.cumulativeOutputLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && shiftedPitch.supportsReset,
            "non-neutral simple pitch shifter should own exact N-1 latency and reset authority");

    auto dynamicDocument = makeSimplePitchShifterOutputDocument();
    dynamicDocument.graph.nodes.push_back(
        node("interval", "talas.constant", {{"value", "0"}}));
    dynamicDocument.graph.connections.push_back(
        connection("interval", "value", "pitch", "interval"));
    const auto dynamic = talas::runtime::compileProject(
        dynamicDocument,
        makeRegistry(),
        {});
    require(dynamic.ok(),
            "connected simple pitch shifter graph should compile");
    const auto& dynamicPitch = requireStep(dynamic.graph->plan(), "pitch");
    require(dynamicPitch.resolvedLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && dynamicPitch.cumulativeOutputLatencyFrames
                == talas::dsp::phaseVocoderPitchLatencyFrames
            && dynamicPitch.supportsReset,
            "connected simple pitch shifter should own fixed N-1 latency even at neutral fallback");
}

void testInvalidPanParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makePanStereoOutputDocument({{"pan", "-0.1"}, {"scale", "1.0"}, choiceParameter("type", "power")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pan", "pan");

    result = talas::runtime::compileProject(
        makePanStereoOutputDocument({{"pan", "1.1"}, {"scale", "1.0"}, choiceParameter("type", "power")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pan", "pan");

    result = talas::runtime::compileProject(
        makePanStereoOutputDocument({{"pan", "0.5"}, {"scale", "-1.0"}, choiceParameter("type", "power")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pan", "scale");
}

void testInvalidDynamicRangeControllerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeDynamicRangeControllerOutputDocument({choiceParameter("type", "compressor"), {"ratio", "0"}, {"threshold", "0.5"}, {"attackTime", "0.001 ms"}, {"releaseTime", "0.001 ms"}, {"delay", "0 s"}, {"gain", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "drc", "ratio");

    result = talas::runtime::compileProject(
        makeDynamicRangeControllerOutputDocument({choiceParameter("type", "compressor"), {"ratio", "2.0"}, {"threshold", "-0.1"}, {"attackTime", "0.001 ms"}, {"releaseTime", "0.001 ms"}, {"delay", "0 s"}, {"gain", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "drc", "threshold");

    result = talas::runtime::compileProject(
        makeDynamicRangeControllerOutputDocument({choiceParameter("type", "compressor"), {"ratio", "2.0"}, {"threshold", "0.5"}, {"attackTime", "0 s"}, {"releaseTime", "0.001 ms"}, {"delay", "0 s"}, {"gain", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "drc", "attackTime");

    result = talas::runtime::compileProject(
        makeDynamicRangeControllerOutputDocument({choiceParameter("type", "compressor"), {"ratio", "2.0"}, {"threshold", "0.5"}, {"attackTime", "0.001 ms"}, {"releaseTime", "0 s"}, {"delay", "0 s"}, {"gain", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "drc", "releaseTime");

    result = talas::runtime::compileProject(
        makeDynamicRangeControllerOutputDocument({choiceParameter("type", "compressor"), {"ratio", "2.0"}, {"threshold", "0.5"}, {"attackTime", "0.001 ms"}, {"releaseTime", "0.001 ms"}, {"delay", "-1 s"}, {"gain", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "drc", "delay");
}

void testInvalidReverbSectionParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeReverbSectionOutputDocument(
            {choiceParameter("type", "comb"),
             {"scale", "1.0"},
             {"decayTime", "0 s"},
             {"delay", "250 ms"},
             {"delayScale", "1.0"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             choiceParameter("interpolation", "linear"),
             booleanParameter("smoothDelayChanges", true)}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "reverb", "decayTime");

    result = talas::runtime::compileProject(
        makeReverbSectionOutputDocument(
            {choiceParameter("type", "comb"),
             {"scale", "1.0"},
             {"decayTime", "1 s"},
             {"delay", "-1 ms"},
             {"delayScale", "1.0"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             choiceParameter("interpolation", "linear"),
             booleanParameter("smoothDelayChanges", true)}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "reverb", "delay");

    result = talas::runtime::compileProject(
        makeReverbSectionOutputDocument(
            {choiceParameter("type", "comb"),
             {"scale", "1.0"},
             {"decayTime", "1 s"},
             {"delay", "250 ms"},
             {"delayScale", "-0.1"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             choiceParameter("interpolation", "linear"),
             booleanParameter("smoothDelayChanges", true)}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "reverb", "delayScale");

    result = talas::runtime::compileProject(
        makeReverbSectionOutputDocument(
            {choiceParameter("type", "comb"),
             {"scale", "1.0"},
             {"decayTime", "1 s"},
             {"delay", "250 ms"},
             {"delayScale", "1.1"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             choiceParameter("interpolation", "linear"),
             booleanParameter("smoothDelayChanges", true)}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "reverb", "delayScale");
}

talas::project::ProjectDocument makeStereoMix2OutputDocument(
    std::vector<talas::project::ProjectParameterExpression> mixParameters = {})
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mix", "talas.stereoMix2", std::move(mixParameters)));
    document.graph.nodes.push_back(node("osc1", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc2", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc1", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(connection("osc2", "audioOut", "mix", "in2"));
    document.graph.connections.push_back(connection("mix", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("mix", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> stereoMix4Parameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"left", "1.0"},
        {"right", "1.0"},
        {"pan1", "0.0"},
        {"scale1", "1.0"},
        {"pan2", "1.0"},
        {"scale2", "1.0"},
        {"pan3", "0.0"},
        {"scale3", "1.0"},
        {"pan4", "1.0"},
        {"scale4", "1.0"},
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeStereoMix4OutputDocument(
    std::vector<talas::project::ProjectParameterExpression> mixParameters = stereoMix4Parameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mix", "talas.stereoMix4", std::move(mixParameters)));
    document.graph.nodes.push_back(node("osc1", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc2", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc3", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc4", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc1", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(connection("osc2", "audioOut", "mix", "in2"));
    document.graph.connections.push_back(connection("osc3", "audioOut", "mix", "in3"));
    document.graph.connections.push_back(connection("osc4", "audioOut", "mix", "in4"));
    document.graph.connections.push_back(connection("mix", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("mix", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> overlappingMixerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        textParameter("legacyInputs", "in1 in3"),
        {"overlapTime", "0 s"},
        {"left", "0.5"},
        {"right", "-0.25"},
        // These fixtures mix every Input at once, as projects before format
        // 46 do.
        choiceParameter("sequence", "legacyAllAtOnce"),
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeOverlappingMixerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> mixParameters = overlappingMixerParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mix", "talas.overlappingMixer", std::move(mixParameters)));
    document.graph.nodes.push_back(node("osc1", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc3", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc1", "audioOut", "mix", "in1"));
    document.graph.connections.push_back(connection("osc3", "audioOut", "mix", "in3"));
    document.graph.connections.push_back(connection("mix", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("mix", "rightOut", "out", "rightIn"));
    return document;
}

talas::project::ProjectDocument makeSilentOverlappingMixerOutputDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("mix", "talas.overlappingMixer", overlappingMixerParameters({
        textParameter("legacyInputs", ""),
        {"left", "1.0"},
        {"right", "1.0"},
    })));
    document.graph.connections.push_back(connection("mix", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("mix", "rightOut", "out", "rightIn"));
    return document;
}

std::vector<talas::project::ProjectParameterExpression> analogSequencerParameters(
    std::vector<talas::project::ProjectParameterExpression> overrides = {})
{
    std::vector<talas::project::ProjectParameterExpression> parameters = {
        {"left", "0.5"},
        {"right", "0.25"},
        {"gate", "1.0"},
        {"step", "0"},
        {"rate", "1.0"},
        booleanParameter("loop", true),
        {"startIndex", "0"},
        {"endIndex", "1"},
        {"polyphony", "1"},
        textParameter("durations", "{1 s} {1 s}"),
        textParameter("dutyCycles", "{0.5} {1}"),
        textParameter("pitches", ""),
        textParameter("velocities", "{0.25} {1}"),
        textParameter("extraValues", ""),
        // Step numbers the stage here, as in projects before format 75.
        choiceParameter("stepMode", "legacyStageNumber"),
    };

    for (auto& override : overrides)
    {
        const auto existing = std::ranges::find_if(parameters, [&](const auto& parameter) {
            return parameter.parameterId == override.parameterId;
        });
        if (existing == parameters.end())
            parameters.push_back(std::move(override));
        else
            *existing = std::move(override);
    }

    return parameters;
}

talas::project::ProjectDocument makeAnalogSequencerOutputDocument(
    std::vector<talas::project::ProjectParameterExpression> sequencerParameters = analogSequencerParameters())
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("seq", "talas.analogSequencer", std::move(sequencerParameters)));
    document.graph.nodes.push_back(node("osc", "talas.oscillator", {{"frequency", "1 hz"}, {"amplitude", "1.0"}}));
    document.graph.connections.push_back(connection("osc", "audioOut", "seq", "audioIn"));
    document.graph.connections.push_back(connection("seq", "leftOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("seq", "rightOut", "out", "rightIn"));
    return document;
}

void testConnectedAnalogSequencerControlsPrecedeVoiceClonesAndPreserveScopes()
{
    auto keyOwnerParameters = [](std::size_t polyphony) {
        const auto keyCount = polyphony == 2 ? 2U : 1U;
        const auto keys = keyCount == 2 ? "#(1 0)" : "#(1)";
        const auto pitches = keyCount == 2 ? "#(60 64)" : "#(67)";
        const auto velocities = keyCount == 2 ? "#(0.8 0.6)" : "#(0.7)";
        const auto timbres = keyCount == 2 ? "#(0.1 0.3)" : "#(0.5)";
        return analogSequencerParameters({
            {"polyphony", std::to_string(polyphony)},
            textParameter("pitches", ""),
            textParameter("velocities", ""),
            textParameter("keyDowns", keys),
            textParameter("keyPitches", pitches),
            textParameter("keyVelocities", velocities),
            textParameter("keyTimbres", timbres),
        });
    };

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("out", "talas.audioOutput"),
        node("sourceOwner", "talas.analogSequencer", keyOwnerParameters(2)),
        node("sourceOsc", "talas.oscillator"),
        node("follower", "talas.amplitudeFollower"),
        node("stepSource", "talas.constant", {{"value", "1.0"}}),
        node("rateSource", "talas.constant", {{"value", "1.5"}}),
        node("targetOwner", "talas.analogSequencer", keyOwnerParameters(1)),
        node("targetOsc", "talas.oscillator"),
    };
    document.graph.connections = {
        connection("sourceOsc", "audioOut", "sourceOwner", "audioIn"),
        connection("sourceOwner", "leftOut", "follower", "audioIn"),
        connection("follower", "value", "targetOwner", "gate"),
        connection("stepSource", "value", "targetOwner", "step"),
        connection("rateSource", "value", "targetOwner", "rate"),
        connection("targetOsc", "audioOut", "targetOwner", "audioIn"),
        connection("targetOwner", "leftOut", "out", "audioIn"),
        connection("targetOwner", "rightOut", "out", "rightIn"),
    };

    const auto registry = makeRegistry();
    const auto original = talas::runtime::compileProject(document, registry, {});
    require(original.ok(),
        "connected key-control owner graph should compile;"
            + compileDiagnosticsSummary(original));

    const auto saved = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved, registry);
    require(reopened.ok(), "connected key-control graph should reopen from saved JSON");
    const auto roundTrip = talas::runtime::compileProject(
        reopened.document, registry, {});
    require(roundTrip.ok(),
        "saved and reopened key-control graph should compile;"
            + compileDiagnosticsSummary(roundTrip));

    const auto& plan = original.graph->plan();
    const auto& reopenedPlan = roundTrip.graph->plan();
    require(plan.midiVoices == reopenedPlan.midiVoices,
        "saved and reopened graph should preserve exact MIDI owner and voice scope identities");
    require(plan.midiVoices.size() == 2,
        "both connected key owners should retain prepared voice plans");

    const auto stepIndex = [&](std::string_view nodeId) {
        const auto found = std::ranges::find(
            plan.steps, nodeId, &talas::runtime::CompileStep::nodeId);
        require(found != plan.steps.end(),
            "compile plan should contain " + std::string(nodeId));
        return static_cast<std::size_t>(std::distance(plan.steps.begin(), found));
    };
    const auto ownerPlan = [&](std::string_view nodeId)
        -> const talas::runtime::MidiVoicePlan& {
        const auto found = std::ranges::find(
            plan.midiVoices, nodeId, &talas::runtime::MidiVoicePlan::ownerNodeId);
        require(found != plan.midiVoices.end(),
            "compile plan should publish a voice plan for "
                + std::string(nodeId));
        return *found;
    };
    const auto clonesImmediatelyPrecedeOwner = [](
        const talas::runtime::MidiVoicePlan& voicePlan) {
        if (voicePlan.voices.empty())
            return false;
        auto expectedFirstStep = voicePlan.voices.front().firstStepIndex;
        for (const auto& voice : voicePlan.voices)
        {
            if (voice.firstStepIndex != expectedFirstStep)
                return false;
            expectedFirstStep += voice.stepCount;
        }
        return expectedFirstStep == voicePlan.ownerStepIndex;
    };
    const auto firstVoiceStep = [](const talas::runtime::MidiVoicePlan& voicePlan) {
        std::size_t first = std::numeric_limits<std::size_t>::max();
        for (const auto& voice : voicePlan.voices)
            first = std::min(first, voice.firstStepIndex);
        return first;
    };

    const auto& sourceOwner = ownerPlan("sourceOwner");
    const auto& targetOwner = ownerPlan("targetOwner");
    require(sourceOwner.voices.size() == 2 && targetOwner.voices.size() == 1,
        "voice plans should retain their configured polyphony");
    require(clonesImmediatelyPrecedeOwner(sourceOwner)
            && clonesImmediatelyPrecedeOwner(targetOwner),
        "each cloned voice island should be contiguous immediately before its owner");
    require(sourceOwner.ownerStepIndex < stepIndex("follower"),
        "upstream key owner mix should precede its connected control producer");
    require(stepIndex("follower") < firstVoiceStep(targetOwner)
            && stepIndex("stepSource") < firstVoiceStep(targetOwner)
            && stepIndex("rateSource") < firstVoiceStep(targetOwner),
        "Gate, Step and Rate producers should precede the target owner's voice clones");
    require(targetOwner.ownerStepIndex < stepIndex("out"),
        "target owner should remain before the terminal output");

    const auto& targetStep = requireStep(plan, "targetOwner");
    require(requireInputBinding(targetStep, "gate").sourceNodeId == "follower"
            && requireInputBinding(targetStep, "step").sourceNodeId == "stepSource"
            && requireInputBinding(targetStep, "rate").sourceNodeId == "rateSource",
        "target owner should retain exact connected Gate, Step and Rate buffer bindings");

    for (const auto& voicePlan : plan.midiVoices)
    {
        const auto& reopenedVoicePlan = *std::ranges::find(
            reopenedPlan.midiVoices,
            voicePlan.ownerNodeId,
            &talas::runtime::MidiVoicePlan::ownerNodeId);
        require(voicePlan.ownerDefinitionId == reopenedVoicePlan.ownerDefinitionId
                && voicePlan.ownerInstancePath
                    == reopenedVoicePlan.ownerInstancePath
                && voicePlan.voices == reopenedVoicePlan.voices,
            "roundtrip should preserve owner path, voice scope IDs, and exact cloned ranges");
    }

    auto rateOnly = makeAnalogSequencerOutputDocument(keyOwnerParameters(1));
    rateOnly.graph.nodes.push_back(node("rateSource", "talas.constant", {{"value", "1.5"}}));
    rateOnly.graph.connections.push_back(connection("rateSource", "value", "seq", "rate"));
    const auto rateOnlyResult = talas::runtime::compileProject(rateOnly, registry, {});
    require(rateOnlyResult.ok(), "Rate alone selects connected key-control ordering");
    const auto& rateOnlyPlan = rateOnlyResult.graph->plan();
    const auto rateProducer = std::ranges::find(rateOnlyPlan.steps, "rateSource", &talas::runtime::CompileStep::nodeId);
    require(rateProducer != rateOnlyPlan.steps.end()
            && static_cast<std::size_t>(std::distance(rateOnlyPlan.steps.begin(), rateProducer))
                < firstVoiceStep(rateOnlyPlan.midiVoices.front()),
        "a lone Rate producer precedes its owner's voice clones");

    const auto noConnectedControls = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(keyOwnerParameters(1)),
        registry,
        {});
    require(noConnectedControls.ok(),
        "key-owner graph without connected controls should compile");
    const auto& legacyOrder = noConnectedControls.graph->plan().midiVoices.front();
    require(legacyOrder.voices.size() == 1
            && legacyOrder.voices.front().firstStepIndex == 0
            && legacyOrder.ownerStepIndex == 1,
        "graphs without connected key controls should retain prepended voice lowering");
}

void testStereoMix2GraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeStereoMix2OutputDocument(), makeRegistry(), {});

    require(result.ok(), "stereo mix2 graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "stereo mix2 left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mix", "stereo mix2 left output buffer should point to mix");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "stereo mix2 left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "stereo mix2 right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "stereo mix2 right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "mix", "stereo mix2 right output buffer should point to mix");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "stereo mix2 right output buffer should point to rightOut");
}

void testStereoMix4GraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeStereoMix4OutputDocument(), makeRegistry(), {});

    require(result.ok(), "stereo mix4 graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "stereo mix4 left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mix", "stereo mix4 left output buffer should point to mix");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "stereo mix4 left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "stereo mix4 right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "stereo mix4 right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "mix", "stereo mix4 right output buffer should point to mix");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "stereo mix4 right output buffer should point to rightOut");

    const auto& mix = requireStep(plan, "mix");
    for (const auto* inputId : {"in1", "in2", "in3", "in4"})
    {
        const auto& input = requireInputBinding(mix, inputId);
        require(input.sourcePortId == "audioOut", std::string(inputId) + " should bind to oscillator audio output");
    }
}

void testOverlappingMixerGraphCompilesWithStereoOutputBuffersAndActiveInputs()
{
    const auto result = talas::runtime::compileProject(makeOverlappingMixerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "overlapping mixer graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "overlapping mixer left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mix", "overlapping mixer left output buffer should point to mix");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "overlapping mixer left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "overlapping mixer right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "overlapping mixer right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "mix", "overlapping mixer right output buffer should point to mix");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "overlapping mixer right output buffer should point to rightOut");

    const auto& mix = requireStep(plan, "mix");
    requireTextParameter(mix, "legacyInputs", "in1 in3");
    requireNear(requireParameter(mix, "overlapTime").value, 0.0, 1.0e-12, "overlap time should compile as zero seconds");
    requireNear(requireParameter(mix, "left").value, 0.5, 1.0e-12, "left output level should compile");
    requireNear(requireParameter(mix, "right").value, -0.25, 1.0e-12, "right output level should compile");

    const auto& input1 = requireInputBinding(mix, "in1");
    const auto& input3 = requireInputBinding(mix, "in3");
    require(input1.sourceNodeId == "osc1", "overlapping mixer in1 should bind to osc1");
    require(input3.sourceNodeId == "osc3", "overlapping mixer in3 should bind to osc3");
    require(
        mix.inputs.size() == 2
            && mix.inputs[0].inputPortId == "in1"
            && mix.inputs[1].inputPortId == "in3",
        "overlapping mixer compile plan should preserve descriptor input order");
    require(
        mix.parameters.size() == 5
            && mix.parameters[0].id == "legacyInputs"
            && mix.parameters[1].id == "overlapTime"
            && mix.parameters[2].id == "left"
            && mix.parameters[3].id == "right"
            && mix.parameters[4].id == "sequence",
        "overlapping mixer compile plan should preserve exact parameter order");
    require(
        mix.offlineSupport == talas::core::ExecutionSupport::Subset
            && mix.offlineBinding == talas::core::ExecutionBinding::LegacyAdapter
            && mix.preparedRealtimeSupport == talas::core::ExecutionSupport::Subset
            && mix.preparedRealtimeBinding == talas::core::ExecutionBinding::LegacyAdapter
            && !mix.supportsReset
            && mix.resolvedLatencyFrames == 0
            && mix.cumulativeInputLatencyFrames == 0
            && mix.cumulativeOutputLatencyFrames == 0
            && !mix.kernelKind.has_value()
            && !mix.preparedIsolatedComponent,
        "overlapping mixer compile plan should preserve exact C23 prepared authority");
}

void testOverlappingMixerGraphCompilesWithNoActiveInputsAsSilence()
{
    const auto result = talas::runtime::compileProject(makeSilentOverlappingMixerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "overlapping mixer graph without active inputs should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "silent overlapping mixer left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "mix", "silent overlapping mixer left output buffer should point to mix");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "silent overlapping mixer left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "silent overlapping mixer right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "silent overlapping mixer right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "mix", "silent overlapping mixer right output buffer should point to mix");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "silent overlapping mixer right output buffer should point to rightOut");

    const auto& mix = requireStep(plan, "mix");
    require(mix.inputs.empty(), "silent overlapping mixer should not require active input bindings");
    requireTextParameter(mix, "legacyInputs", "");
    requireNear(requireParameter(mix, "left").value, 1.0, 1.0e-12, "silent overlapping mixer left output level should compile");
    requireNear(requireParameter(mix, "right").value, 1.0, 1.0e-12, "silent overlapping mixer right output level should compile");
}

void testAnalogSequencerGraphCompilesWithStereoOutputBuffers()
{
    const auto result = talas::runtime::compileProject(makeAnalogSequencerOutputDocument(), makeRegistry(), {});

    require(result.ok(), "analog sequencer graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "analog sequencer left output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "seq", "analog sequencer left output buffer should point to seq");
    require(plan.buffers[plan.outputBufferIndex].portId == "leftOut", "analog sequencer left output buffer should point to leftOut");
    require(plan.rightOutputBufferIndex.has_value(), "analog sequencer right output buffer index should be present");
    require(*plan.rightOutputBufferIndex < plan.buffers.size(), "analog sequencer right output buffer index should be valid");
    require(plan.buffers[*plan.rightOutputBufferIndex].nodeId == "seq", "analog sequencer right output buffer should point to seq");
    require(plan.buffers[*plan.rightOutputBufferIndex].portId == "rightOut", "analog sequencer right output buffer should point to rightOut");

    const auto& seq = requireStep(plan, "seq");
    requireNear(requireParameter(seq, "left").value, 0.5, 1.0e-12, "analog sequencer left should compile");
    requireNear(requireParameter(seq, "right").value, 0.25, 1.0e-12, "analog sequencer right should compile");
    requireNear(requireParameter(seq, "gate").value, 1.0, 1.0e-12, "analog sequencer gate should compile");
    requireNear(requireParameter(seq, "step").value, 0.0, 1.0e-12, "analog sequencer step should compile");
    requireNear(requireParameter(seq, "rate").value, 1.0, 1.0e-12, "analog sequencer rate should compile");
    requireBooleanParameter(seq, "loop", true);
    requireNear(requireParameter(seq, "startIndex").value, 0.0, 1.0e-12, "analog sequencer startIndex should compile");
    requireNear(requireParameter(seq, "endIndex").value, 1.0, 1.0e-12, "analog sequencer endIndex should compile");
    requireNear(requireParameter(seq, "polyphony").value, 1.0, 1.0e-12, "analog sequencer polyphony should compile");
    requireTextParameter(seq, "durations", "{1 s} {1 s}");
    requireTextParameter(seq, "dutyCycles", "{0.5} {1}");
    requireTextParameter(seq, "pitches", "");
    requireTextParameter(seq, "velocities", "{0.25} {1}");
    requireTextParameter(seq, "extraValues", "");

    const auto& input = requireInputBinding(seq, "audioIn");
    require(input.sourceNodeId == "osc", "analog sequencer audioIn should bind oscillator");
    require(input.sourcePortId == "audioOut", "analog sequencer audioIn should bind oscillator output");
}

void testOscilloscopeDisplayGraphCompilesAsPassthrough()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection("scope", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "oscilloscope display graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "oscilloscope display output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "scope", "oscilloscope display output buffer should point to scope");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "oscilloscope display output buffer should point to audioOut");
}

void testOscilloscopeDisplayBindsDirectSoundTrigger()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {{"sweepFreq", "2 hz"}}));
    document.graph.nodes.push_back(node("displayed", "talas.oscillator"));
    document.graph.nodes.push_back(node("pulse", "talas.oscillator"));
    document.graph.connections.push_back(connection("displayed", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection("pulse", "audioOut", "scope", "sweepOrTrig"));
    document.graph.connections.push_back(connection("scope", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.enableRealtimeAnalysisCapture = true;
    const auto result = talas::runtime::compileProject(document, makeRegistry(), settings);
    require(result.ok(), "oscilloscope display with direct Sound trigger should compile");
    const auto& plan = result.graph->plan();
    const auto scope = std::ranges::find_if(plan.steps, [](const auto& step) {
        return step.nodeId == "scope";
    });
    require(scope != plan.steps.end(), "direct-trigger scope should have a compiled step");
    const auto& trigger = requireInputBinding(*scope, "sweepOrTrig");
    require(trigger.sourceNodeId == "pulse" && trigger.sourcePortId == "audioOut",
        "SweepOrTrig should bind the pasted audio Sound directly");
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->ready(), "direct trigger should prepare analysis capture");
    const auto triggerProbe = capture->probeIndex(
        "scope", "sweepOrTrigOut", scope->scopeId,
        talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock);
    require(triggerProbe.has_value(), "direct trigger should have a dedicated prepared capture probe");
    const auto* spec = capture->probeSpec(*triggerProbe);
    require(spec != nullptr && spec->nodeId == "scope"
            && spec->portId == "sweepOrTrigOut",
        "direct trigger probe should capture the scope-owned copy of its source Sound");
    const auto displayedProbe = capture->preferredProbeIndex("scope");
    const auto* displayedSpec = displayedProbe.has_value()
        ? capture->probeSpec(*displayedProbe)
        : nullptr;
    require(displayedSpec != nullptr
            && displayedSpec->sweepFrequencyHz == 2.0
            && displayedSpec->sourceInputPortId == "audioIn"
            && displayedSpec->triggerModeEnabled
            && !displayedSpec->triggerOnFallingEdge,
        "prepared scope should retain SweepFreq, pre-Level input, and direct-trigger rising default");
    require(capture->preferredTriggerProbeIndex(*displayedProbe)
            == triggerProbe,
        "pasted direct Sound should remain the selected trigger probe");
}

void testOscilloscopeDisplayRejectsOutOfRangeSweepFrequency()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {{"sweepFreq", "0.05 hz"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection(
        "scope", "audioOut", "out", "audioIn"));
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(!result.ok(),
        "scope should reject SweepFreq outside its bounded display range");
}

void testOscilloscopeDisplayPreparesSubAudioInitialSweepFrequency()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {{"initialSweepFrequency", "0.25 hz"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection(
        "scope", "audioOut", "out", "audioIn"));
    talas::runtime::RenderSettings settings{1024.0, 8};
    settings.enableRealtimeAnalysisCapture = true;
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    require(result.ok(), "sub-audio InitialSweepFrequency should compile");
    const auto capture = result.graph->analysisCapture();
    const auto primary = capture->preferredProbeIndex("scope");
    require(primary.has_value()
            && capture->probeSpec(*primary)->sampleStrideFrames == 1
            && capture->probeSpec(*primary)->initialSweepFrequencyHz == 0.25,
        "trigger capture should retain full-rate audio and authored timebase");
    const auto slow = capture->probeIndex(
        "scope", "@slow-sweep", capture->probeSpec(*primary)->scopeId,
        talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock);
    require(slow.has_value()
            && capture->probeSpec(*slow)->sourceInputPortId == "audioIn"
            && capture->probeSpec(*slow)->sampleStrideFrames == 16,
        "prepared slow sweep should span one 0.25 Hz cycle across 256 samples");
}

void testOscilloscopeDisplayRejectsOutOfRangeLevel()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {{"level", "4.1"}}));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection(
        "scope", "audioOut", "out", "audioIn"));
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(!result.ok(),
        "scope should reject Level outside its bounded post-display range");
}

void testOscilloscopeDisplayBindsRightTriggerChannel()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {booleanParameter("trigSwap", true)}));
    document.graph.nodes.push_back(node("left", "talas.oscillator"));
    document.graph.nodes.push_back(node("right", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "left", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection(
        "right", "audioOut", "scope", "rightIn"));
    document.graph.connections.push_back(connection(
        "scope", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection(
        "scope", "rightOut", "out", "rightIn"));

    talas::runtime::RenderSettings settings;
    settings.enableRealtimeAnalysisCapture = true;
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    require(result.ok(), "stereo OscilloscopeDisplay should compile");
    const auto& plan = result.graph->plan();
    const auto scope = std::ranges::find_if(plan.steps, [](const auto& step) {
        return step.nodeId == "scope";
    });
    require(scope != plan.steps.end(), "stereo scope should have a compiled step");
    const auto& right = requireInputBinding(*scope, "rightIn");
    require(right.sourceNodeId == "right"
            && right.sourcePortId == "audioOut",
        "scope right input should bind the right Sound");
    requireBooleanParameter(*scope, "trigSwap", true);
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->ready(),
        "stereo scope should prepare an analysis capture");
    const auto displayed = capture->preferredProbeIndex("scope");
    const auto rightProbe = capture->probeIndex(
        "scope", "rightOut", scope->scopeId,
        talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock);
    require(displayed.has_value() && rightProbe.has_value()
            && *displayed != *rightProbe
            && capture->probeSpec(*displayed)->triggerOnRight
            && capture->probeSpec(*rightProbe)->sourceInputPortId == "rightIn",
        "TrigSwap should retain distinct pre-Level right trigger and left display probes");

    document.graph.connections.erase(
        std::ranges::find_if(document.graph.connections,
            [](const auto& edge) {
                return edge.toPort == "rightIn"
                    && edge.toNode == "scope";
            }));
    const auto missingRight = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    require(!missingRight.ok(),
        "TrigSwap should reject a Sound without a right input channel");
}

void testOscilloscopeDisplayAuthorsFallingTriggerMode()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay",
        {
            choiceParameter("sweepOrTrig", "trigger"),
            choiceParameter("trigThresh", "falling"),
        }));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection(
        "osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection(
        "scope", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.enableRealtimeAnalysisCapture = true;
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    require(result.ok(), "authored falling trigger mode should compile");
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->ready(),
        "authored trigger mode should prepare capture");
    const auto displayed = capture->preferredProbeIndex("scope");
    const auto* spec = displayed.has_value()
        ? capture->probeSpec(*displayed) : nullptr;
    require(spec != nullptr && spec->triggerModeEnabled
            && spec->triggerOnFallingEdge
            && !capture->preferredTriggerProbeIndex(*displayed).has_value(),
        "Workbench should use the displayed input for authored falling-edge trigger");
}

void testSpectrumAnalyzerDisplayGraphCompilesAsPassthrough()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("analyzer", "talas.spectrumAnalyzerDisplay"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "analyzer", "audioIn"));
    document.graph.connections.push_back(connection("analyzer", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    require(result.ok(), "spectrum analyzer display graph should compile");
    const auto& plan = result.graph->plan();
    require(plan.outputBufferIndex < plan.buffers.size(), "spectrum analyzer display output buffer index should be valid");
    require(plan.buffers[plan.outputBufferIndex].nodeId == "analyzer", "spectrum analyzer display output buffer should point to analyzer");
    require(plan.buffers[plan.outputBufferIndex].portId == "audioOut", "spectrum analyzer display output buffer should point to audioOut");
}

void testDisplayGraphCompilesPreparedAnalysisProbesWhenEnabled()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("annotation", "talas.annotation"));
    document.graph.nodes.push_back(node("analyzer", "talas.spectrumAnalyzerDisplay"));
    document.graph.nodes.push_back(node("scope", "talas.oscilloscopeDisplay"));
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc", "audioOut", "scope", "audioIn"));
    document.graph.connections.push_back(connection("scope", "audioOut", "analyzer", "audioIn"));
    document.graph.connections.push_back(connection("analyzer", "audioOut", "annotation", "audioIn"));
    document.graph.connections.push_back(connection("annotation", "audioOut", "out", "audioIn"));
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 32;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisQueueCapacity = 3;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "scope",
        "audioOut",
        talas::runtime::RealtimeAnalysisKind::Oscilloscope,
        talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock,
        1,
        "workspace-scope",
    });

    const auto result = talas::runtime::compileProject(document, makeRegistry(), std::move(settings));

    require(result.ok(), "analysis-enabled display graph should compile");
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->ready(), "analysis-enabled graph should own ready capture");
    require(capture->maxBlockFrames()
                == talas::runtime::realtimeOscilloscopeHistoryFrames
            && capture->queueCapacity() == 3,
            "prepared scope capture should reserve its rolling history while preserving queue depth");
    require(capture->probeCount() == 2, "annotation should not create an analysis probe");
    const auto scope = capture->preferredProbeIndex("scope");
    const auto analyzer = capture->preferredProbeIndex("analyzer");
    require(scope.has_value()
                && capture->probeSpec(*scope)->kind == talas::runtime::RealtimeAnalysisKind::Oscilloscope
                && capture->probeIndexForClientId("workspace-scope")
                    == scope,
            "workspace target should attach its client identity to the identical prepared display probe");
    require(analyzer.has_value()
                && capture->probeSpec(*analyzer)->kind == talas::runtime::RealtimeAnalysisKind::Spectrum,
            "spectrum step should prepare a spectrum probe");
    require(!capture->preferredProbeIndex("annotation").has_value(),
            "annotation should remain transparent without capture");
}

void testExactNestedAnalysisTargetCompilesToOnePreparedProbe()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    voice.graph.nodes.push_back(node("osc", "talas.oscillator"));
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode voiceInstance;
    voiceInstance.instanceId = "voice1";
    voiceInstance.soundDefinitionId = "voice";
    document.graph.nodes = {
        std::move(voiceInstance),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections.push_back(
        connection("voice1", "audioOut", "out", "audioIn"));

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 32;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {"voice1"},
        "voice",
        "osc",
        "audioOut",
        talas::runtime::RealtimeAnalysisKind::Oscilloscope,
        talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock,
        1,
        "trace-voice-osc",
    });
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), std::move(settings));
    require(result.ok(), "exact nested Analysis target should compile");
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->probeCount() == 1,
        "exact nested Analysis target should prepare one probe");
    const auto* probe = capture->probeSpec(0);
    require(probe != nullptr
            && probe->scopeId != talas::runtime::globalRuntimeScope
            && probe->portId == "audioOut"
            && probe->clientId == "trace-voice-osc"
            && probe->payloadKind
                == talas::runtime::RealtimeAnalysisPayloadKind::AudioBlock,
        "nested Analysis target should resolve to exact prepared scope and port");

    talas::runtime::RealtimeRenderSession session(*result.graph);
    require(session.ready() && session.bindAnalysisStage(91),
        "nested Analysis runtime should bind its exact stage");
    std::array<double, 4> rendered{};
    require(session.render(rendered),
        "nested Analysis target should not affect graph rendering");
    std::array<double, 32> captured{};
    const auto record = capture->copyLatestRecord(0, captured);
    require(record.has_value() && record->stageId == 91
            && record->valueCount == rendered.size(),
        "nested Analysis target should publish one complete callback record");
    for (std::size_t frame = 0; frame < rendered.size(); ++frame)
        requireNear(captured[frame], rendered[frame], 1.0e-12,
            "nested Analysis capture should match exact prepared node output");
}

void testAnalysisTargetResolutionFailsClosed()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator"),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections.push_back(
        connection("osc", "audioOut", "out", "audioIn"));
    talas::runtime::RenderSettings settings;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "missing",
        "audioOut",
    });
    const auto missing = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    requireCompileFailure(
        missing,
        DiagnosticCode::GraphValidationFailed,
        "missing");

    settings.realtimeAnalysisTargets.front().nodeId = "osc";
    settings.realtimeAnalysisTargets.front().portId = "missingOut";
    const auto badPort = talas::runtime::compileProject(
        document, makeRegistry(), settings);
    requireCompileFailure(
        badPort,
        DiagnosticCode::GraphValidationFailed,
        "osc");

    settings.enableRealtimeAnalysisCapture = false;
    const auto disabled = talas::runtime::compileProject(
        document, makeRegistry(), std::move(settings));
    requireCompileFailure(disabled, DiagnosticCode::InvalidRenderSettings);
}

void testExactTypedEventAnalysisTargetRequiresDescriptorAuthority()
{
    auto document = makeAdsrOutputDocument();
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 32;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "env",
        std::string{talas::runtime::realtimeAnalysisTypedEventEndpoint},
        talas::runtime::RealtimeAnalysisKind::Oscilloscope,
        talas::runtime::RealtimeAnalysisPayloadKind::EventBatch,
        1,
        "trace-env-events",
    });

    const auto accepted = talas::runtime::compileProject(
        document,
        makeRegistry(),
        settings);
    require(accepted.ok(),
        "descriptor-declared typed Event Analysis endpoint should compile");
    const auto capture = accepted.graph->analysisCapture();
    const auto* probe = capture != nullptr
        ? capture->probeSpec(0)
        : nullptr;
    require(capture != nullptr
            && capture->ready()
            && capture->probeCount() == 1
            && capture->maxPayloadValues()
                >= talas::runtime::maximumRealtimeAnalysisEventCount * 3
            && probe != nullptr
            && probe->nodeId == "env"
            && probe->portId
                == talas::runtime::realtimeAnalysisTypedEventEndpoint
            && probe->payloadKind
                == talas::runtime::RealtimeAnalysisPayloadKind::EventBatch,
        "typed Event target should prepare one exact bounded node endpoint");

    auto conflictingClient = settings;
    conflictingClient.realtimeAnalysisTargets.push_back(
        conflictingClient.realtimeAnalysisTargets.front());
    conflictingClient.realtimeAnalysisTargets.back().clientId =
        "trace-env-events-conflict";
    const auto conflicting = talas::runtime::compileProject(
        document,
        makeRegistry(),
        std::move(conflictingClient));
    requireCompileFailure(
        conflicting,
        DiagnosticCode::GraphValidationFailed,
        "env");

    settings.realtimeAnalysisTargets.front().nodeId = "osc";
    const auto unsupportedNode = talas::runtime::compileProject(
        document,
        makeRegistry(),
        settings);
    requireCompileFailure(
        unsupportedNode,
        DiagnosticCode::GraphValidationFailed,
        "osc");

    settings.realtimeAnalysisTargets.front().nodeId = "env";
    settings.realtimeAnalysisTargets.front().portId = "value";
    const auto fabricatedPort = talas::runtime::compileProject(
        document,
        makeRegistry(),
        std::move(settings));
    requireCompileFailure(
        fabricatedPort,
        DiagnosticCode::GraphValidationFailed,
        "env");
}

void testExactControlAnalysisTargetPublishesPreparedValues()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        node("osc", "talas.oscillator"),
        node("gain", "talas.constant", {{"value", "0.25"}}),
        node("vca", "talas.vca"),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {
        connection("osc", "audioOut", "vca", "audioIn"),
        connection("gain", "value", "vca", "gain"),
        connection("vca", "audioOut", "out", "audioIn"),
    };
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 16;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "gain",
        "value",
        talas::runtime::RealtimeAnalysisKind::Oscilloscope,
        talas::runtime::RealtimeAnalysisPayloadKind::ControlBlock,
        1,
        "trace-gain",
    });
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), std::move(settings));
    require(result.ok(), "exact Control Analysis target should compile");
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->probeCount() == 1,
        "Control Analysis target should prepare one exact probe");
    talas::runtime::RealtimeRenderSession session(*result.graph);
    require(session.ready() && session.bindAnalysisStage(92),
        "Control Analysis target should bind its exact stage");
    std::array<double, 8> rendered{};
    require(session.render(rendered),
        "Control Analysis target should not affect rendering");
    std::array<double, 16> captured{};
    const auto record = capture->copyLatestRecord(0, captured);
    require(record.has_value()
            && record->payloadKind
                == talas::runtime::RealtimeAnalysisPayloadKind::ControlBlock
            && record->stageId == 92
            && record->valueCount == rendered.size(),
        "Control Analysis target should publish a typed stage-bound record");
    for (std::size_t frame = 0; frame < rendered.size(); ++frame)
    {
        requireNear(captured[frame], 0.25, 1.0e-12,
            "Control Analysis record should preserve exact prepared values");
    }
}

void testExactPartialSpectrumAnalysisTargetPublishesPreparedFrame()
{
    auto document = makeLiveSpectralAnalysisTrackSelectorOutputDocument();
    const auto input = std::ranges::find(
        document.graph.nodes,
        std::string_view{"input"},
        &talas::project::ProjectNode::instanceId);
    require(input != document.graph.nodes.end(),
        "PartialSpectrum Analysis fixture should contain its input node");
    *input = node(
        "input",
        "talas.oscillator",
        {{"frequency", "3000 hz"}, {"amplitude", "1.0"}});

    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 64;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "spectrum",
        "spectrumOut",
        talas::runtime::RealtimeAnalysisKind::Spectrum,
        talas::runtime::RealtimeAnalysisPayloadKind::PartialSpectrum,
        1,
        "trace-partials",
    });
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), std::move(settings));
    require(result.ok(),
        "exact PartialSpectrum Analysis target should compile;"
            + compileDiagnosticsSummary(result));
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->probeCount() == 1
            && capture->maxPayloadValues()
                >= talas::dsp::maximumLiveSpectralPartials * 3,
        "PartialSpectrum Analysis target should prepare its exact bounded payload capacity");
    talas::runtime::RealtimeRenderSession session(*result.graph);
    require(session.ready(),
        "PartialSpectrum Analysis target should prepare its realtime session: "
            + std::string{session.error()});
    require(session.bindAnalysisStage(93),
        "PartialSpectrum Analysis target should bind its exact stage");
    std::array<double, 64> rendered{};
    require(session.render(rendered),
        "PartialSpectrum Analysis target should not affect rendering");
    const auto sourceFrames = session.inspectPartialSpectrum(
        "spectrum", "spectrumOut");
    require(!sourceFrames.empty() && !sourceFrames.back().partials.empty(),
        "PartialSpectrum Analysis fixture should produce a prepared frame");
    std::array<double, talas::runtime::maximumRealtimeAnalysisPayloadValues>
        captured{};
    const auto record = capture->copyLatestRecord(0, captured);
    const auto& source = sourceFrames.back();
    require(record.has_value()
            && record->payloadKind
                == talas::runtime::RealtimeAnalysisPayloadKind::PartialSpectrum
            && record->stageId == 93
            && record->absoluteSamplePosition
                == source.sourceTimestamp.absoluteFrame
            && record->valueCount == source.partials.size() * 3,
        "PartialSpectrum Analysis target should publish the newest exact prepared frame");
    for (std::size_t partial = 0; partial < source.partials.size(); ++partial)
    {
        requireNear(captured[partial * 3],
            source.partials[partial].frequencyHz, 1.0e-12,
            "PartialSpectrum Analysis record should preserve frequency");
        requireNear(captured[partial * 3 + 1],
            source.partials[partial].amplitude, 1.0e-12,
            "PartialSpectrum Analysis record should preserve amplitude");
        requireNear(captured[partial * 3 + 2],
            source.partials[partial].bandwidthHz, 1.0e-12,
            "PartialSpectrum Analysis record should preserve bandwidth");
    }
}

void testExactComplexAnalysisTargetPublishesPreparedFrame()
{
    talas::runtime::RenderSettings settings;
    settings.maxBlockFrames = 64;
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisTargets.push_back({
        {},
        {},
        "forward",
        "spectrumOut",
        talas::runtime::RealtimeAnalysisKind::Spectrum,
        talas::runtime::RealtimeAnalysisPayloadKind::ComplexFrame,
        1,
        "trace-complex",
    });
    const auto result = talas::runtime::compileProject(
        makeNativeStftOutputDocument(),
        makeRegistry(),
        std::move(settings));
    require(result.ok(),
        "exact ComplexFrame Analysis target should compile;"
            + compileDiagnosticsSummary(result));
    const auto capture = result.graph->analysisCapture();
    require(capture != nullptr && capture->probeCount() == 1
            && capture->maxPayloadValues() == 66,
        "ComplexFrame Analysis target should budget 33 interleaved complex bins");
    talas::runtime::RealtimeRenderSession session(*result.graph);
    require(session.ready() && session.bindAnalysisStage(94),
        "ComplexFrame Analysis target should bind its exact stage");
    std::array<double, 64> rendered{};
    require(session.render(rendered),
        "ComplexFrame Analysis target should not affect rendering");
    std::array<double, talas::runtime::maximumRealtimeAnalysisPayloadValues>
        captured{};
    const auto record = capture->copyLatestRecord(0, captured);
    require(record.has_value()
            && record->payloadKind
                == talas::runtime::RealtimeAnalysisPayloadKind::ComplexFrame
            && record->stageId == 94
            && record->valueCount == 66,
        "ComplexFrame Analysis target should publish 33 interleaved real/imaginary bins");
    require(std::ranges::all_of(
                std::span<const double>{captured}.first(record->valueCount),
                [](double value) { return std::isfinite(value); }),
        "ComplexFrame Analysis payload should contain only finite prepared bin values");
}

void testInvalidStereoMix2ParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"left", "-1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "left");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"left", "1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "left");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"right", "-1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "right");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"right", "1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "right");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"pan1", "-0.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "pan1");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"pan2", "1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "pan2");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"scale1", "-0.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "scale1");

    result = talas::runtime::compileProject(makeStereoMix2OutputDocument({{"scale2", "1.1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "scale2");
}

void testInvalidStereoMix4ParametersFailCompile()
{
    const std::vector<std::pair<std::string, std::string>> invalidParameters = {
        {"left", "-1.1"},
        {"right", "1.1"},
        {"pan1", "-0.1"},
        {"pan2", "1.1"},
        {"pan3", "-0.1"},
        {"pan4", "1.1"},
        {"scale1", "-0.1"},
        {"scale2", "1.1"},
        {"scale3", "-0.1"},
        {"scale4", "1.1"},
    };

    for (const auto& [parameterId, expression] : invalidParameters)
    {
        const auto result = talas::runtime::compileProject(
            makeStereoMix4OutputDocument(stereoMix4Parameters({{parameterId, expression}})),
            makeRegistry(),
            {});
        requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", parameterId);
    }
}

void testInvalidOverlappingMixerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeOverlappingMixerOutputDocument(overlappingMixerParameters({{"overlapTime", "1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "overlapTime");

    result = talas::runtime::compileProject(
        makeOverlappingMixerOutputDocument(overlappingMixerParameters({textParameter("legacyInputs", "in5")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "legacyInputs");

    result = talas::runtime::compileProject(
        makeOverlappingMixerOutputDocument(overlappingMixerParameters({textParameter("legacyInputs", "in1 in4")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "legacyInputs");

    result = talas::runtime::compileProject(
        makeOverlappingMixerOutputDocument(overlappingMixerParameters({{"left", "1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "left");

    result = talas::runtime::compileProject(
        makeOverlappingMixerOutputDocument(overlappingMixerParameters({{"right", "-1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mix", "right");
}

void testInvalidAnalogSequencerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"left", "-1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "left");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"right", "1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "right");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "gate");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"step", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "step");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"rate", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "rate");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"startIndex", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "startIndex");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"endIndex", "2.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "endIndex");

    result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(analogSequencerParameters({{"polyphony", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "seq", "polyphony");
}

void testInvalidForcedProcessorAssignmentParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "0"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "forced", "processor");

    result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "-1"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "forced", "processor");

    result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "1.5"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "forced", "processor");

    result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "1.0000000001"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "forced", "processor");

    result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "4294967296"}}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "forced", "processor");

    result = talas::runtime::compileProject(makeForcedProcessorAssignmentOutputDocument({{"processor", "4294967295"}}), makeRegistry(), {});
    require(result.ok(), "UINT32_MAX processor should compile");
}

void testInvalidPulseTrainParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makePulseTrainOutputDocument({{"period", "0 s"}, booleanParameter("variableDutyCycle", false), {"dutyCycle", "0.5"}, {"gate", "1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "period");

    result = talas::runtime::compileProject(
        makePulseTrainOutputDocument({{"period", "-1 s"}, booleanParameter("variableDutyCycle", false), {"dutyCycle", "0.5"}, {"gate", "1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "period");

    result = talas::runtime::compileProject(
        makePulseTrainOutputDocument({{"period", "1 s"}, booleanParameter("variableDutyCycle", true), {"dutyCycle", "-0.1"}, {"gate", "1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "dutyCycle");

    result = talas::runtime::compileProject(
        makePulseTrainOutputDocument({{"period", "1 s"}, booleanParameter("variableDutyCycle", true), {"dutyCycle", "1.1"}, {"gate", "1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "dutyCycle");
}

void testInvalidPulseGeneratorParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makePulseGeneratorOutputDocument({
            {"frequency", "-1 hz"},
            textParameter("modulation", ""),
            {"maxMI", "0"},
            choiceParameter("interpolation", "linear"),
            {"envelope", "1.0"},
            {"dutyCycle", "0.5"},
            {"reset", "0"},
        }),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::NegativeFrequency, "pulse", "frequency");

    result = talas::runtime::compileProject(
        makePulseGeneratorOutputDocument({
            {"frequency", "1 hz"},
            textParameter("modulation", ""),
            {"maxMI", "0"},
            choiceParameter("interpolation", "linear"),
            {"envelope", "1.0"},
            {"dutyCycle", "-0.1"},
            {"reset", "0"},
        }),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "dutyCycle");

    result = talas::runtime::compileProject(
        makePulseGeneratorOutputDocument({
            {"frequency", "1 hz"},
            textParameter("modulation", ""),
            {"maxMI", "0"},
            choiceParameter("interpolation", "linear"),
            {"envelope", "1.0"},
            {"dutyCycle", "1.1"},
            {"reset", "0"},
        }),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pulse", "dutyCycle");
}

void testInvalidIteratedWaveshaperParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeIteratedWaveshaperOutputDocument({{"iterations", "0"}, {"scale", "0.5"}, resourceParameter("wavetable", "Tables/Dynamics")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "iterations");

    result = talas::runtime::compileProject(
        makeIteratedWaveshaperOutputDocument({{"iterations", "1.5"}, {"scale", "0.5"}, resourceParameter("wavetable", "Tables/Dynamics")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "iterations");

    result = talas::runtime::compileProject(
        makeIteratedWaveshaperOutputDocument({{"iterations", "4097"}, {"scale", "0.5"}, resourceParameter("wavetable", "Tables/Dynamics")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "iterations");

    result = talas::runtime::compileProject(
        makeIteratedWaveshaperOutputDocument({{"iterations", "2"}, {"scale", "-0.1"}, resourceParameter("wavetable", "Tables/Dynamics")}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "scale");
}

void testInvalidShelvingFilterCutoffFailsCompile()
{
    auto result = talas::runtime::compileProject(
        makeShelvingFilterOutputDocument(
            "highShelf",
            "talas.highShelvingFilter",
            {{"cutoffFreq", "0 hz"}, {"boostOrCut", "6"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "highShelf", "cutoffFreq");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeShelvingFilterOutputDocument(
            "lowShelf",
            "talas.lowShelvingFilter",
            {{"cutoffFreq", "4000 hz"}, {"boostOrCut", "6"}, {"scale", "1.0"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lowShelf", "cutoffFreq");
}

void testInvalidPresenceFilterParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makePresenceFilterOutputDocument({{"centerFreq", "0 hz"}, {"bandwidth", "200 hz"}, {"boostOrCut", "6"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "presence", "centerFreq");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makePresenceFilterOutputDocument({{"centerFreq", "4000 hz"}, {"bandwidth", "200 hz"}, {"boostOrCut", "6"}, {"scale", "1.0"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "presence", "centerFreq");

    result = talas::runtime::compileProject(
        makePresenceFilterOutputDocument({{"centerFreq", "500 hz"}, {"bandwidth", "0 hz"}, {"boostOrCut", "6"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "presence", "bandwidth");
}

void testInvalidTwoFormantElementParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeTwoFormantElementOutputDocument(
            {{"formant1", "0 hz"},
             {"bandwidth1", "100 hz"},
             {"scale1", "1.0"},
             {"formant2", "1500 hz"},
             {"bandwidth2", "200 hz"},
             {"scale2", "0.5"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "formants", "formant1");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeTwoFormantElementOutputDocument(
            {{"formant1", "500 hz"},
             {"bandwidth1", "100 hz"},
             {"scale1", "1.0"},
             {"formant2", "4000 hz"},
             {"bandwidth2", "200 hz"},
             {"scale2", "0.5"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "formants", "formant2");

    result = talas::runtime::compileProject(
        makeTwoFormantElementOutputDocument(
            {{"formant1", "500 hz"},
             {"bandwidth1", "0 hz"},
             {"scale1", "1.0"},
             {"formant2", "1500 hz"},
             {"bandwidth2", "200 hz"},
             {"scale2", "0.5"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "formants", "bandwidth1");

    result = talas::runtime::compileProject(
        makeTwoFormantElementOutputDocument(
            {{"formant1", "500 hz"},
             {"bandwidth1", "100 hz"},
             {"scale1", "1.0"},
             {"formant2", "1500 hz"},
             {"bandwidth2", "0 hz"},
             {"scale2", "0.5"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "formants", "bandwidth2");
}

void testInvalidTwoFormantVoiceElementParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "0 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "4000 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "frequency");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "0 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "formant1");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "4000 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "formant2");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "0 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "bandwidth1");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "-1 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "bandwidth2");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1 / 0"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "scale1");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "1 / 0"}, {"seed", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "scale2");

    result = talas::runtime::compileProject(
        makeTwoFormantVoiceElementOutputDocument({{"frequency", "200 hz"}, {"formant1", "500 hz"}, {"bandwidth1", "100 hz"}, {"scale1", "1"}, {"formant2", "1500 hz"}, {"bandwidth2", "200 hz"}, {"scale2", "0.5"}, {"seed", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "voice", "seed");
}

void testInvalidHarmonicResonatorParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeHarmonicResonatorOutputDocument(
            {{"decayTime", "0 s"},
             {"brightness", "1.0"},
             {"frequency", "4 hz"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "decayTime");

    result = talas::runtime::compileProject(
        makeHarmonicResonatorOutputDocument(
            {{"decayTime", "1 s"},
             {"brightness", "1.0"},
             {"frequency", "0 hz"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "frequency");

    result = talas::runtime::compileProject(
        makeHarmonicResonatorOutputDocument(
            {{"decayTime", "1 s"},
             {"brightness", "-0.1"},
             {"frequency", "4 hz"},
             resourceParameter("wavetable", "Private"),
             booleanParameter("prezero", true),
             {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "brightness");
}

void testInvalidReResonatorParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeReResonatorOutputDocument({resourceParameter("wavetable", "Tables/RE"), {"timeIndex", "-1.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "timeIndex");

    result = talas::runtime::compileProject(
        makeReResonatorOutputDocument({resourceParameter("wavetable", "Tables/RE"), {"timeIndex", "1.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "timeIndex");

    result = talas::runtime::compileProject(
        makeReResonatorOutputDocument({resourceParameter("wavetable", "Tables/RE"), {"timeIndex", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "resonator", "timeIndex");
}

void testInvalidGraphicEqParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeGraphicEqOutputDocument(
            {{"cf250Hz", "1.0"},
             {"cf500Hz", "-0.1"},
             {"cf1000Hz", "1.0"},
             {"cf2000Hz", "1.0"},
             {"cf4000Hz", "1.0"},
             {"cf8000Hz", "1.0"},
             {"cf16000Hz", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "eq", "cf500Hz");

    result = talas::runtime::compileProject(
        makeGraphicEqOutputDocument(
            {{"cf250Hz", "1.0"},
             {"cf500Hz", "1.0"},
             {"cf1000Hz", "1 / 0"},
             {"cf2000Hz", "1.0"},
             {"cf4000Hz", "1.0"},
             {"cf8000Hz", "1.0"},
             {"cf16000Hz", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "eq", "cf1000Hz");
}

void testInvalidVcfParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1.0"}, {"cutoff", "0 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "100 hz"}, {"resonance", "0.25"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "cutoff");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1.0"}, {"cutoff", "4000 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "100 hz"}, {"resonance", "0.25"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "cutoff");

    result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1.0"}, {"cutoff", "500 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "-1 hz"}, {"resonance", "0.25"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "cutoffModRange");

    result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1.0"}, {"cutoff", "500 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "100 hz"}, {"resonance", "-0.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "resonance");

    result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1.0"}, {"cutoff", "500 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "100 hz"}, {"resonance", "1.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "resonance");

    result = talas::runtime::compileProject(
        makeVcfOutputDocument({{"amplitude", "1 / 0"}, {"cutoff", "500 hz"}, {"cutoffModulator", "0.0"}, {"cutoffModRange", "100 hz"}, {"resonance", "0.25"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vcf", "amplitude");
}

void testInvalidSingleSideBandRmParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({{"frequency", "0 hz"}, {"freqScale", "2.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ssb", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({{"frequency", "4000 hz"}, {"freqScale", "2.0"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ssb", "frequency");

    result = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({{"frequency", "440 hz"}, {"freqScale", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ssb", "freqScale");

    result = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({{"frequency", "440 hz"}, {"freqScale", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ssb", "freqScale");
}

void testInvalidPhaseShiftBy90ParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makePhaseShiftBy90OutputDocument({{"frequency", "0 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "phase", "frequency");

    result = talas::runtime::compileProject(
        makePhaseShiftBy90OutputDocument({{"frequency", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "phase", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makePhaseShiftBy90OutputDocument({{"frequency", "4000 hz"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "phase", "frequency");
}

void testInvalidAnalysisFilterParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "0 hz"}, {"harmonic", "2"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "fundamental");

    result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "-1 hz"}, {"harmonic", "2"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "fundamental");

    result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "100 hz"}, {"harmonic", "-0.5"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "harmonic");

    result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "100 hz"}, {"harmonic", "-1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "harmonic");

    result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "100 hz"}, {"harmonic", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "harmonic");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument({{"fundamental", "1000 hz"}, {"harmonic", "4"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "analysis", "harmonic");
}

void testInvalidSumOfSinesSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"onDuration", "0 s"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "onDuration");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"frequency0", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "frequency0");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"frequency1", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "frequency1");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"analysisFrequency0", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "analysisFrequency0");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"analysisFrequency1", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "analysisFrequency1");

    auto dynamicAnalysisSettings = talas::runtime::RenderSettings{};
    dynamicAnalysisSettings.eventValues.push_back({
        "C61AnalysisFrequency", 125.0, talas::expr::ValueDomain::Hertz});
    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            {"analysisFrequency0", "!C61AnalysisFrequency"},
        })),
        makeRegistry(),
        std::move(dynamicAnalysisSettings));
    // Analysis metadata is a constant field: an
    // Event Value there compiles to its starting value.
    require(result.ok(), "an Event Value analysisFrequency0 should compile to its starting value");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"frequency0", "5000 hz"}})),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "frequency0");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"dbMorph", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "dbMorph");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"pchMorph", "1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "pchMorph");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"nbrPartials", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"nbrPartials", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"bankSize", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "bankSize");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"timeIndex", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "timeIndex");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"analysisFrames0", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "analysisFrames0");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"analysisFrames1", "4097"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "analysisFrames1");

    auto dynamicFramesSettings = talas::runtime::RenderSettings{};
    dynamicFramesSettings.eventValues.push_back({
        "C62AnalysisFrames", 3.0, talas::expr::ValueDomain::Scalar});
    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            {"analysisFrames0", "!C62AnalysisFrames"},
            booleanParameter("ctrlTime", true),
        })),
        makeRegistry(),
        std::move(dynamicFramesSettings));
    require(result.ok(), "an Event Value analysisFrames0 should compile to its starting value");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "gate");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            booleanParameter("loop", true),
            {"loopStart", "0.75"},
            {"loopEnd", "0.25"},
        })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "loopEnd");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            booleanParameter("loop", true),
            {"loopStart", "-0.01"},
            {"loopEnd", "0.75"},
        })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "loopStart");

    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            booleanParameter("loop", true),
            {"loopStart", "0.25"},
            {"loopEnd", "1.01"},
        })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "loopEnd");

    auto dynamicLoopSettings = talas::runtime::RenderSettings{};
    dynamicLoopSettings.eventValues.push_back({
        "C63LoopStart", 0.25, talas::expr::ValueDomain::Scalar});
    result = talas::runtime::compileProject(
        makeSumOfSinesOutputDocument(sumOfSinesParameters({
            booleanParameter("loop", true),
            {"loopStart", "!C63LoopStart"},
            {"loopEnd", "0.75"},
        })),
        makeRegistry(),
        std::move(dynamicLoopSettings));
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sum", "loopStart");

}

void testInvalidGaOscillatorsParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"timeIndex", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "timeIndex");

    result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"frequency", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8000.0;
    result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"frequency", "4000 hz"}})),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "frequency");

    result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"envelope", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "envelope");

    result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"morph", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "morph");

    result = talas::runtime::compileProject(
        makeGaOscillatorsOutputDocument(gaOscillatorsParameters({{"morph", "1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "ga", "morph");
}

void testInvalidFilterBankSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeFilterBankOutputDocument(filterBankParameters({{"nbrFilters", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "nbrFilters");

    result = talas::runtime::compileProject(
        makeFilterBankOutputDocument(filterBankParameters({{"bankSize", "0.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "bankSize");

    // A fractional BankSize, as default * 0.75 can give, counts its whole
    // units.
    result = talas::runtime::compileProject(
        makeFilterBankOutputDocument(filterBankParameters({{"bankSize", "1.5"}})),
        makeRegistry(),
        {});
    require(result.ok(), "a fractional FilterBank bankSize should compile");
    const auto& steps = result.graph->plan().steps;
    const auto bank = std::ranges::find(steps, std::string_view{"bank"}, &talas::runtime::CompileStep::nodeId);
    require(bank != steps.end(), "the FilterBank should have a compile step");
    const auto bankSize = std::ranges::find(bank->parameters, std::string_view{"bankSize"}, &talas::runtime::ParameterValue::id);
    require(bankSize != bank->parameters.end() && bankSize->value == 1.0,
        "a FilterBank bankSize of 1.5 should count one whole filter per processor");

    result = talas::runtime::compileProject(
        makeFilterBankOutputDocument(filterBankParameters({{"bandwidth", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "bandwidth");

    result = talas::runtime::compileProject(
        makeFilterBankElementOutputDocument(filterBankElementParameters({{"first", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "element", "first");

    result = talas::runtime::compileProject(
        makeFilterBankElementOutputDocument(filterBankElementParameters({{"count", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "element", "count");

    result = talas::runtime::compileProject(
        makeFilterBankElementOutputDocument(filterBankElementParameters({{"bandwidth", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "element", "bandwidth");
}

void testInvalidFormantBankSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeFormantBankOutputDocument(formantBankParameters({{"frequency", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "frequency");

    result = talas::runtime::compileProject(
        makeFormantBankOutputDocument(formantBankParameters({{"nbrFormants", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "nbrFormants");

    const auto requireBankOverlapCompileFailure = [&](std::vector<talas::project::ProjectParameterExpression> overrides) {
        const auto invalid = talas::runtime::compileProject(
            makeFormantBankOutputDocument(formantBankParameters(std::move(overrides))),
            makeRegistry(),
            {});
        requireCompileFailure(invalid, DiagnosticCode::ParameterEvaluationError, "bank", "nbrImpulses");
    };
    requireBankOverlapCompileFailure({{"nbrImpulses", "0"}});
    requireBankOverlapCompileFailure({{"nbrImpulses", "1.5"}});
    requireBankOverlapCompileFailure({{"nbrImpulses", "4097"}});
    requireBankOverlapCompileFailure({{"nbrFormants", "17"}, {"nbrImpulses", "4096"}});

    result = talas::runtime::compileProject(
        makeFormantBankOscillatorOutputDocument(formantBankOscillatorParameters({{"firstFormant", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "formant", "firstFormant");

    const auto requireOscillatorOverlapCompileFailure = [&](std::vector<talas::project::ProjectParameterExpression> overrides) {
        const auto invalid = talas::runtime::compileProject(
            makeFormantBankOscillatorOutputDocument(formantBankOscillatorParameters(std::move(overrides))),
            makeRegistry(),
            {});
        requireCompileFailure(invalid, DiagnosticCode::ParameterEvaluationError, "formant", "nbrImpulses");
    };
    requireOscillatorOverlapCompileFailure({{"nbrImpulses", "0"}});
    requireOscillatorOverlapCompileFailure({{"nbrImpulses", "1.5"}});
    requireOscillatorOverlapCompileFailure({{"nbrImpulses", "4097"}});
    requireOscillatorOverlapCompileFailure({{"nbrFormants", "17"}, {"nbrImpulses", "4096"}});

}

void testInvalidVocoderSubsetParametersFailCompile()
{
    // More scale steps than bands: the bands take the first ones.
    auto result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.scaleVocoder", scaleVocoderParameters({
            textParameter("intervals", "0 4 7"), {"sideLevel", "1"}})),
        makeRegistry(),
        {});
    require(result.ok(), "a scale with more steps than bands should compile");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"},
                {"bankSize", "2"},
                textParameter("intervals", "0 nope"),
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "intervals");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"},
                {"bankSize", "2"},
                textParameter("intervals", "0 0"),
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "intervals");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"},
                {"bankSize", "2"},
                textParameter("intervals", "0 12"),
                {"sideLevel", "1"},
                {"tonic", "10000 nn"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "intervals");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"},
                {"bankSize", "2"},
                textParameter("intervals", "0 12"),
                {"sideLevel", "1"},
                {"inBandwidth", "0"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "bankSize");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "129"},
                {"bankSize", "129"},
                textParameter("intervals", std::string(129, '0')),
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "bankSize");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "1"},
                {"bankSize", "1"},
                textParameter(
                    "intervals",
                    std::string(
                        talas::dsp::maximumScaleVocoderIntervalTextBytes + 1,
                        '0')),
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "intervals");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"},
                {"bankSize", "2"},
                textParameter("intervals", "0 12"),
                {"sideLevel", "1"},
                {"tone", "1.1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "tone");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.scaleVocoder",
            scaleVocoderParameters({
                {"nbrBands", "2"}, {"bankSize", "2"},
                textParameter("intervals", "0 12"),
                {"sideLevel", "1"}, {"rolloff", "4.1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "rolloff");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.scaleVocoder", scaleVocoderParameters({{"sideLevel", "1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "intervals");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.tunableVocoder", tunableVocoderParameters({{"analysisSpacing", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "analysisSpacing");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.tunableVocoder", tunableVocoderParameters({{"analysisLevel", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "analysisLevel");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.tunableVocoder",
            tunableVocoderParameters({
                {"nbrBands", "129"},
                {"bankSize", "129"},
                {"analysisLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "nbrBands");

    // BankSize only spreads the filters over expansion cards.
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.tunableVocoder",
            tunableVocoderParameters({
                {"nbrBands", "4"},
                {"bankSize", "3"},
                {"analysisLevel", "1"},
            })),
        makeRegistry(),
        {});
    require(result.ok(), "a TunableVocoder BankSize below NbrBands should compile: "
        + compileDiagnosticsSummary(result));

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.tunableVocoder",
            tunableVocoderParameters({
                {"nbrBands", "16"},
                {"bankSize", "16"},
                booleanParameter("logSpacing", true),
                {"analysisLevel", "1"},
                {"analysisSpacing", "100"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "analysisSpacing");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.tunableVocoder",
            tunableVocoderParameters({
                {"analysisLevel", "1"},
                {"tone", "-1.1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "tone");

    // Pitch spaces the bands equally in pitch.
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.vocoder", vocoderParameters({booleanParameter("pitch", true)})),
        makeRegistry(),
        {});
    require(result.ok(), "a Vocoder spaced in pitch should compile: " + compileDiagnosticsSummary(result));

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument("talas.vocoder", vocoderParameters({{"lowCF", "2000 hz"}, {"highCF", "1000 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "lowCF");

    result = talas::runtime::compileProject(
        makeVocoderChannelBankOutputDocument(vocoderChannelBankParameters({{"first", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "first");

    result = talas::runtime::compileProject(
        makeVocoderChannelBankOutputDocument(
            vocoderChannelBankParameters({{"count", "129"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "bank", "count");

    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.vocoder",
            vocoderParameters({
                {"nbrBands", "129"},
                {"bankSize", "65"},
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "nbrBands");

    // BankSize only spreads the filters over expansion cards.
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.vocoder",
            vocoderParameters({
                {"nbrBands", "4"},
                {"bankSize", "3"},
                {"sideLevel", "1"},
            })),
        makeRegistry(),
        {});
    require(result.ok(), "a Vocoder BankSize below NbrBands should compile: "
        + compileDiagnosticsSummary(result));

    auto lowRate = talas::runtime::RenderSettings{};
    lowRate.sampleRate = 4000.0;
    result = talas::runtime::compileProject(
        makeVocoderOutputDocument(
            "talas.vocoder",
            vocoderParameters({
                {"sideLevel", "1"},
                {"lowCF", "100 hz"},
                {"highCF", "3000 hz"},
            })),
        makeRegistry(),
        lowRate);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "vocoder", "highCF");
}

void testInvalidBlankScriptFamilySubsetParametersFailCompile()
{
    // ADR-0015 amendment: a grammar that does not parse, or a field
    // compiling cannot fix, fails to compile.
    auto result = talas::runtime::compileProject(
        makeContextFreeGrammarOutputDocument(contextFreeGrammarParameters({
            textParameter("rewriteRules", "A -> B"), textParameter("startExpression", "(A)")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "grammar", "rewriteRules");

    result = talas::runtime::compileProject(
        makeContextFreeGrammarOutputDocument(contextFreeGrammarParameters({textParameter("startExpression", "(s1 ,")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "grammar", "startExpression");

    result = talas::runtime::compileProject(
        makeContextFreeGrammarOutputDocument(contextFreeGrammarParameters({
            textParameter("startExpression", "(s1)"), {"maxRewrites", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "grammar", "maxRewrites");

    result = talas::runtime::compileProject(
        makeLimeInterpreterOutputDocument(limeInterpreterParameters({resourceParameter("fileName", "Scripts/main.lime")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lime", "fileName");

    result = talas::runtime::compileProject(
        makeLimeInterpreterOutputDocument(limeInterpreterParameters({textParameter("script", "1 +")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lime", "script");

    result = talas::runtime::compileProject(
        makeLimeInterpreterOutputDocument(limeInterpreterParameters({{"left", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lime", "left");

    result = talas::runtime::compileProject(
        makeLimeInterpreterOutputDocument(limeInterpreterParameters({{"left", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lime", "left");

    result = talas::runtime::compileProject(
        makeLimeInterpreterOutputDocument(limeInterpreterParameters({{"right", "-1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "lime", "right");
}

void testInvalidFeedbackLoopOutputSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeFeedbackLoopOutputDocument(feedbackLoopOutputParameters({textParameter("connection", "loopA")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::GraphValidationFailed, "feedback", "connection");

    result = talas::runtime::compileProject(
        makeFeedbackLoopOutputDocument(feedbackLoopOutputParameters({{"delay", "1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "feedback", "delay");
}

void testInvalidScriptSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeScriptOutputDocument(scriptParameters({textParameter("script", "output := audioIn")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "script", "script");

    result = talas::runtime::compileProject(makeScriptOutputDocument(scriptParameters({{"left", "1 / 0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "script", "left");

    result = talas::runtime::compileProject(makeScriptOutputDocument(scriptParameters({{"right", "0 / 0"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "script", "right");

    result = talas::runtime::compileProject(makeScriptOutputDocument(scriptParameters({{"left", "1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "script", "left");

    result = talas::runtime::compileProject(makeScriptOutputDocument(scriptParameters({{"right", "-1.5"}})), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "script", "right");
}

void testInvalidParameterTransformerSubsetParametersFailCompile()
{
    const auto result = talas::runtime::compileProject(
        makeParameterTransformerOutputDocument({textParameter("transformation", "freq := freq * 2")}),
        makeRegistry(),
        {});

    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "transform", "transformation");
}

void testInvalidFftSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "0"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "2"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "8"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "12"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "1.5"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "8192"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "-1"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "17"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "4097"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");

    result = talas::runtime::compileProject(makeFftOutputDocument({{"length", "1 / 0"}, booleanParameter("inverse", false)}), makeRegistry(), {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "fft", "length");
}

void testInvalidSyntheticSpectrumSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSyntheticSpectrumTrackSelectorOutputDocument(syntheticSpectrumFromArrayParameters({{"nbrPartials", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSyntheticSpectrumTrackSelectorOutputDocument(syntheticSpectrumFromArrayParameters({{"nbrPartials", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "nbrPartials");

}

void testInvalidSyntheticSpectrumFromSoundsSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(syntheticSpectrumFromSoundsParameters({{"nbrPartials", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSyntheticSpectrumFromSoundsTrackSelectorOutputDocument(syntheticSpectrumFromSoundsParameters({{"nbrPartials", "4097"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "nbrPartials");

}

void testInvalidLiveSpectralAnalysisSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({textParameter("response", "smooth")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "response");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({booleanParameter("harmonic", true)})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "harmonic");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"unpitchedOnly", "1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "unpitchedOnly");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"trackedHarmonic", "2"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "trackedHarmonic");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"unpitchedThreshold", "0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "unpitchedThreshold");

    // Band tracks take a harmonic analysis within its
    // ranges.
    const auto harmonicBands = [](std::vector<talas::project::ProjectParameterExpression> overrides) {
        overrides.push_back(choiceParameter("tracks", "bands"));
        overrides.push_back(booleanParameter("harmonic", true));
        return makeLiveSpectralAnalysisTrackSelectorOutputDocument(
            liveSpectralAnalysisParameters(std::move(overrides)));
    };
    for (const auto& [parameterId, value] : std::initializer_list<std::pair<std::string, std::string>>{
             {"trackedHarmonic", "0"}, {"trackedHarmonic", "1.5"}, {"trackedHarmonic", "65"},
             {"unpitchedOnly", "0.5"}, {"unpitchedThreshold", "-0.1"}, {"unpitchedThreshold", "1.5"},
             {"highFreq", "24000 hz"}, {"initFreq", "500 hz"}})
    {
        result = talas::runtime::compileProject(harmonicBands({{parameterId, value}}), makeRegistry(), {});
        requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", parameterId);
    }
    result = talas::runtime::compileProject(
        harmonicBands({{"trackedHarmonic", "2"}, {"unpitchedOnly", "1"}, {"unpitchedThreshold", "0.5"}}),
        makeRegistry(),
        {});
    require(result.ok(), "a harmonic LiveSpectralAnalysis over bands should compile;" + compileDiagnosticsSummary(result));

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"ampScale", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "ampScale");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"freqScale", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "freqScale");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"lowFreq", "13000 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "lowFreq");

    result = talas::runtime::compileProject(
        makeLiveSpectralAnalysisTrackSelectorOutputDocument(liveSpectralAnalysisParameters({{"initFreq", "500 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "spectrum", "initFreq");
}

void testInvalidSpectrumFileSourceSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"frequency", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "frequency");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"level", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "level");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"timeIndex", "-1.25"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "timeIndex");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"timeIndex", "1.25"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "timeIndex");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"analysisFrames", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"analysisFrames", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"analysisFrames", "4097"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"analysisFrequency", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrequency");

    auto dynamicAnalysisSettings = talas::runtime::RenderSettings{};
    dynamicAnalysisSettings.eventValues.push_back({
        "C59AnalysisFrequency", 125.0, talas::expr::ValueDomain::Hertz});
    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({
            {"analysisFrequency", "!C59AnalysisFrequency"},
        })),
        makeRegistry(),
        std::move(dynamicAnalysisSettings));
    // Analysis metadata is constant: an Event Value
    // compiles to its starting value (2026-10-13).
    require(result.ok(), "SpectrumInRAM analysisFrequency given an Event Value should compile to its starting value");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"nbrPartials", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSpectrumInRamTrackSelectorOutputDocument(spectrumInRamParameters({{"firstPartial", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "firstPartial");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"frequency", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "frequency");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"analysisFrequency", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrequency");

    auto dynamicDiskMetadataSettings = talas::runtime::RenderSettings{};
    dynamicDiskMetadataSettings.eventValues.push_back({
        "C60AnalysisFrequency", 125.0, talas::expr::ValueDomain::Hertz});
    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({
            {"analysisFrequency", "!C60AnalysisFrequency"},
        })),
        makeRegistry(),
        std::move(dynamicDiskMetadataSettings));
    // Analysis metadata is constant: an Event Value
    // compiles to its starting value (2026-10-13).
    require(result.ok(), "SpectrumOnDisk analysisFrequency given an Event Value should compile to its starting value");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"level", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "level");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"rateScale", "-0.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "rateScale");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"trigger", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "trigger");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"analysisFrames", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"analysisFrames", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"analysisFrames", "4097"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "analysisFrames");

    auto dynamicLayoutSettings = talas::runtime::RenderSettings{};
    dynamicLayoutSettings.eventValues = {
        {"C56AnalysisFrames", 2.0, talas::expr::ValueDomain::Scalar},
    };
    dynamicLayoutSettings.eventValueChanges = {
        {1, "C56AnalysisFrames", 3.0, talas::expr::ValueDomain::Scalar},
    };
    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(
            spectrumOnDiskParameters({
                {"analysisFrames", "!C56AnalysisFrames"},
            })),
        makeRegistry(),
        dynamicLayoutSettings);
    // It plays its starting layout; the scheduled change is never read.
    require(result.ok() && result.graph.has_value(),
        "scheduled C56 analysisFrames should compile to their starting value");

    auto directLiveSettings = talas::runtime::RenderSettings{};
    directLiveSettings.eventValues = {
        {"C56Scalar", 1.0, talas::expr::ValueDomain::Scalar},
        {"C56Frequency", 100.0, talas::expr::ValueDomain::Hertz},
    };
    // Frequency, Level, RateScale and Trigger follow their Event Values once
    // per callback; the partial selection stays fixed, compiled to
    // its starting value.
    const std::array directLiveParameters = {
        std::pair{"frequency", "!C56Frequency"},
        std::pair{"level", "!C56Scalar"},
        std::pair{"rateScale", "!C56Scalar"},
        std::pair{"trigger", "!C56Scalar"},
    };
    for (const auto& [parameterId, expression] : directLiveParameters)
    {
        result = talas::runtime::compileProject(
            makeSpectrumOnDiskTrackSelectorOutputDocument(
                spectrumOnDiskParameters({{parameterId, expression}})),
            makeRegistry(),
            directLiveSettings);
        require(result.ok() && result.graph.has_value(),
            std::string{"C56 SpectrumOnDisk should follow an Event Value in "}
                + parameterId);
    }
    const std::array fixedSelectionParameters = {
        std::pair{"nbrPartials", "!C56Scalar"},
        std::pair{"firstPartial", "!C56Scalar"},
    };
    for (const auto& [parameterId, expression] : fixedSelectionParameters)
    {
        result = talas::runtime::compileProject(
            makeSpectrumOnDiskTrackSelectorOutputDocument(
                spectrumOnDiskParameters({{parameterId, expression}})),
            makeRegistry(),
            directLiveSettings);
        require(result.ok() && result.graph.has_value(),
            std::string{"C56 SpectrumOnDisk should compile a moving "} + parameterId
                + " to its starting value");
    }

    auto variableLayoutDocument =
        makeSpectrumOnDiskTrackSelectorOutputDocument(
            spectrumOnDiskParameters({{"analysisFrames", "@c56Frames"}}));
    variableLayoutDocument.variableLibrary.variables.push_back({
        "c56Frames",
        "C56 Frames",
        talas::expr::ValueDomain::Scalar,
        2.0,
    });
    result = talas::runtime::compileProject(
        variableLayoutDocument,
        makeRegistry(),
        {});
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "source",
        "analysisFrames");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(
            spectrumOnDiskParameters({
                {"analysisFrames", "previous(2, 2)"},
            })),
        makeRegistry(),
        {});
    require(result.ok(), "a stateful SpectrumOnDisk analysisFrames should compile to its starting value");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"nbrPartials", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"nbrPartials", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSpectrumOnDiskTrackSelectorOutputDocument(spectrumOnDiskParameters({{"firstPartial", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "firstPartial");
}

void testInvalidSpectrumTrackSelectorSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSyntheticSpectrumTrackSelectorOutputDocument(syntheticSpectrumFromArrayParameters(), spectrumTrackSelectorParameters({{"track", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "selector", "track");

    result = talas::runtime::compileProject(
        makeSyntheticSpectrumTrackSelectorOutputDocument(syntheticSpectrumFromArrayParameters(), spectrumTrackSelectorParameters({{"track", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "selector", "track");
}

void testInvalidSpectrumFrequencyScaleSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSpectrumFrequencyScaleTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumFrequencyScaleParameters({{"scale", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "scale");

    result = talas::runtime::compileProject(
        makeSpectrumFrequencyScaleTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumFrequencyScaleParameters({{"scale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "scale");
}

void testInvalidSpectralShapeSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSpectralShapeTrackSelectorOutputDocument(spectralShapeParameters({{"frequency", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "frequency");

    result = talas::runtime::compileProject(
        makeSpectralShapeTrackSelectorOutputDocument(spectralShapeParameters({{"spacing", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "spacing");

    result = talas::runtime::compileProject(
        makeSpectralShapeTrackSelectorOutputDocument(spectralShapeParameters({{"nbrPartials", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSpectralShapeTrackSelectorOutputDocument(spectralShapeParameters({{"nbrPartials", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "nbrPartials");

    result = talas::runtime::compileProject(
        makeSpectralShapeTrackSelectorOutputDocument(spectralShapeParameters({{"scale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "shape", "scale");
}

void testInvalidSpectrumModifierSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"loTrack", "3"}, {"hiTrack", "2"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "modifier", "loTrack");

    result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"freqHysteresis", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "modifier", "freqHysteresis");

    result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"ampHysteresis", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "modifier", "ampHysteresis");

    result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"probability", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "modifier", "probability");

    result = talas::runtime::compileProject(
        makeSpectrumModifierTrackSelectorOutputDocument(
            syntheticSpectrumFromArrayParameters(),
            spectrumModifierParameters({{"seed", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "modifier", "seed");
}

void testInvalidBlankMidiInSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters({textParameter("map", "60 -> sound")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "map");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument(
            "talas.midiMapper",
            midiMapperParameters({textParameter("map", "Gate")})),
        makeRegistry(),
        {});
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "midi",
        "map");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument(
            "talas.midiMapper",
            midiMapperParameters({
                textParameter(
                    "map",
                    std::string(
                        talas::runtime::maximumPreparedMidiMapperMapBytes + 1,
                        ' ')),
            })),
        makeRegistry(),
        {});
    requireCompileFailure(
        result,
        DiagnosticCode::ParameterEvaluationError,
        "midi",
        "map");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters({textParameter("script", "noteOn")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "script");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({textParameter("script", "noteOn")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "script");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({resourceParameter("midiFile", "Sequences/Part.mid")})),
        makeRegistry(),
        {});
    require(result.ok(), "Live MIDI must retain an inactive file reference without requiring its resource");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({
            textParameter("source", "MIDI File"), resourceParameter("midiFile", "Sequences/Part.mid")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, {}, "midiFile");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({textParameter("source", "MIDI File")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "midiFile");

    // A shared Mapper takes its notes along with other Sounds.
    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters({booleanParameter("shared", true)})),
        makeRegistry(),
        {});
    require(result.ok(), "a shared MIDI Mapper should compile");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({{"polyphony", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "polyphony");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters({{"lowPitch", "80 nn"}, {"highPitch", "40 nn"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "lowPitch");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiMapper", midiMapperParameters({{"left", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "left");

    result = talas::runtime::compileProject(
        makeMidiInOutputDocument("talas.midiVoice", midiVoiceParameters({{"right", "-1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "midi", "right");
}

void testInvalidNoAudioTerminalSubsetParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiFileEcho", midiFileEchoParameters({resourceParameter("fileName", "Sequences/Part.mid")})),
        makeRegistry(),
        {});
    require(result.ok(), "MIDIFileEcho fileName should compile for render-time binary resource loading");
    requireResourceParameter(requireStep(result.graph->plan(), "terminal"), "fileName", "Sequences/Part.mid");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiFileEcho", midiFileEchoParameters({{"lowChannel", "12"}, {"highChannel", "3"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "lowChannel");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputController", midiOutputControllerParameters({{"controllerNumber", "128"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "controllerNumber");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputEvent", midiOutputEventParameters({{"channel", "17"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "channel");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.midiOutputEventInBytes", midiOutputEventInBytesParameters({textParameter("bytes", "90 40 7f")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "bytes");

    result = talas::runtime::compileProject(
        makeNoAudioTerminalOutputDocument("talas.soundToGlobalController", soundToGlobalControllerParameters({textParameter("value", "InputAmp")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "terminal", "value");
}

void testInvalidStateSinkTerminalSubsetParametersFailCompile()
{
    const auto result = talas::runtime::compileProject(
        makeFeedbackLoopInputTerminalOutputDocument(feedbackLoopInputParameters({textParameter("connection", "loopA")})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::MissingAudioOutput);
}

void testInvalidFrequencyScaleParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "1 / 0"}, {"maxScale", "1.0"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "frequencyScale");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "0.49"}, {"maxScale", "2.0"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "frequencyScale");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "2.01"}, {"maxScale", "3.0"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "frequencyScale");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "1.0"}, {"maxScale", "1 / 0"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "maxScale");

    // MaxScale is the largest FrequencyScale, 0.5 for an octave down.
    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "0.5"}, {"maxScale", "0.5"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    require(result.ok(), "a MaxScale of 0.5 should admit an octave down");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "1.0"}, {"maxScale", "0"}, resourceParameter("window", "Private"), {"delay", "0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "maxScale");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "1.0"}, {"maxScale", "1.0"}, resourceParameter("window", "Private"), {"delay", "-0.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "delay");

    result = talas::runtime::compileProject(
        makeFrequencyScaleOutputDocument({{"frequencyScale", "1.0"}, {"maxScale", "1.0"}, resourceParameter("window", "Private"), {"delay", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "scale", "delay");
}

void testInvalidTimeFrequencyScaleParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"frequencyScale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "frequencyScale");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"rate", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "rate");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"rate", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "rate");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({
            {"frequencyScale", "1.0"}, {"rate", "0.249"}})),
        makeRegistry(),
        {});
    require(result.ok(), "positive TimeFrequencyScale rate must be independent of the pitch correction ratio");

    // MaxFreqScale is the largest scale applied to the frequency.
    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({
            {"frequencyScale", "1.5"}, {"rate", "1.0"}, {"maxFreqScale", "1.0"}})),
        makeRegistry(),
        {});
    require(result.ok(), "a TimeFrequencyScale frequencyScale above maxFreqScale should apply maxFreqScale");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"rate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "rate");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "gate");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"minInputFreq", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "minInputFreq");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"minInputFreq", "1000 hz"}, {"maxInputFreq", "500 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "maxInputFreq");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"maxFreqScale", "0.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "maxFreqScale");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"detectors", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "detectors");

    result = talas::runtime::compileProject(
        makeTimeFrequencyScaleOutputDocument(timeFrequencyScaleParameters({{"detectors", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tfs", "detectors");
}

void testInvalidSimplePitchShifterParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "1 / 0"}, {"minInputPitch", "20 hz"}, {"maxInputPitch", "2000 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "interval");

    result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "13"}, {"minInputPitch", "20 hz"}, {"maxInputPitch", "2000 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "interval");

    result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "-13"}, {"minInputPitch", "20 hz"}, {"maxInputPitch", "2000 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "interval");

    result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "0"}, {"minInputPitch", "0 hz"}, {"maxInputPitch", "2000 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "minInputPitch");

    result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "0"}, {"minInputPitch", "20 hz"}, {"maxInputPitch", "20 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "maxInputPitch");

    result = talas::runtime::compileProject(
        makeSimplePitchShifterOutputDocument({{"interval", "0"}, {"minInputPitch", "20 hz"}, {"maxInputPitch", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "pitch", "maxInputPitch");
}

void testInvalidMonotonizerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({{"frequency", "0 hz"}, {"minInputPitch", "440 hz"}, {"maxInputPitch", "440 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mono", "frequency");

    result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({{"frequency", "440 hz"}, {"minInputPitch", "0 hz"}, {"maxInputPitch", "440 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mono", "minInputPitch");

    // A tracked range runs from MinInputPitch up to MaxInputPitch,
    // within an octave of Frequency.
    result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({{"frequency", "440 hz"}, {"minInputPitch", "440 hz"}, {"maxInputPitch", "220 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mono", "maxInputPitch");

    result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({{"frequency", "440 hz"}, {"minInputPitch", "100 hz"}, {"maxInputPitch", "880 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mono", "frequency");

    result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({{"frequency", "219 hz"}, {"minInputPitch", "440 hz"}, {"maxInputPitch", "440 hz"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "mono", "frequency");

    // A range following an Event Value is read each block, as one a Sound
    // feeds, so it keeps the vocoder's latency even while it holds the
    // known pitch.
    auto dynamicSourceSettings = talas::runtime::RenderSettings{};
    dynamicSourceSettings.eventValues = {
        {"KnownPitch", 440.0, talas::expr::ValueDomain::Hertz},
    };
    result = talas::runtime::compileProject(
        makeMonotonizerOutputDocument({
            {"frequency", "440 hz"},
            {"minInputPitch", "!KnownPitch"},
            {"maxInputPitch", "440 hz"},
        }),
        makeRegistry(),
        dynamicSourceSettings);
    require(result.ok(), "a Monotonizer range following an Event Value should compile: "
        + compileDiagnosticsSummary(result));
    require(requireStep(result.graph->plan(), "mono").resolvedLatencyFrames
            == talas::dsp::phaseVocoderPitchLatencyFrames,
        "a Monotonizer range following an Event Value should keep the vocoder's latency");
}

void testInvalidQuadOscillatorParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeQuadOscillatorOutputDocument({{"frequency", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "quad", "frequency");

    auto settings = talas::runtime::RenderSettings{};
    settings.sampleRate = 8.0;
    result = talas::runtime::compileProject(
        makeQuadOscillatorOutputDocument({{"frequency", "4 hz"}}),
        makeRegistry(),
        settings);
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "quad", "frequency");
}

void testInvalidGenericSourceParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeGenericSourceOutputDocument(genericSourceParameters({{"trigger", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "trigger");

    result = talas::runtime::compileProject(
        makeGenericSourceOutputDocument(genericSourceParameters({{"attackTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "attackTime");

    result = talas::runtime::compileProject(
        makeGenericSourceOutputDocument(genericSourceParameters({{"releaseTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "releaseTime");

    result = talas::runtime::compileProject(
        makeGenericSourceOutputDocument(genericSourceParameters({{"scale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "scale");

    result = talas::runtime::compileProject(
        makeGenericSourceOutputDocument(genericSourceParameters({{"frequency", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "source", "frequency");
}

void testInvalidTimeControlParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeTimeControlOutputDocument({{"rate", "-0.1"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "time", "rate");

    result = talas::runtime::compileProject(
        makeTimeControlOutputDocument({{"rate", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "time", "rate");
}

void testInvalidFrequencyTrackerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"minFrequency", "0 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "minFrequency");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"minFrequency", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "minFrequency");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"maxFrequency", "1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "maxFrequency");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"confidence", "1.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "confidence");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"scale", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "scale");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"detectors", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "detectors");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"detectors", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "detectors");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"emphasis", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "emphasis");

    result = talas::runtime::compileProject(
        makeFrequencyTrackerOutputDocument(frequencyTrackerParameters({{"emphasis", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "tracker", "emphasis");
}

void testConnectedFrequencyTrackerControlsOwnInvalidFallbackRanges()
{
    auto document = makeFrequencyTrackerOutputDocument(
        frequencyTrackerParameters({
            {"minFrequency", "0 hz"},
            {"maxFrequency", "100000 hz"},
            {"confidence", "2"},
            {"scale", "-1"},
        }));
    document.graph.nodes.push_back(node(
        "minControl", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "maxControl", "talas.constant", {{"value", "8"}}));
    document.graph.nodes.push_back(node(
        "confidenceControl", "talas.constant", {{"value", "0.5"}}));
    document.graph.nodes.push_back(node(
        "scaleControl", "talas.constant", {{"value", "0.25"}}));
    document.graph.connections.push_back(connection(
        "minControl", "value", "tracker", "minFrequency"));
    document.graph.connections.push_back(connection(
        "maxControl", "value", "tracker", "maxFrequency"));
    document.graph.connections.push_back(connection(
        "confidenceControl", "value", "tracker", "confidence"));
    document.graph.connections.push_back(connection(
        "scaleControl", "value", "tracker", "scale"));

    auto settings = talas::runtime::RenderSettings{64.0, 128};
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), settings);

    require(
        result.ok(),
        "C90 connected Frequency Tracker controls should own range validation over inert invalid fallbacks");
}

void testConnectedAveragingLowPassCutoffOwnsInvalidFallbackRange()
{
    auto document = makeAveragingLowPassFilterOutputDocument({
        {"cutoff", "0 hz"},
        resourceParameter("wavetable", "Private"),
        {"scale", "1"},
    });
    document.graph.nodes.push_back(node(
        "cutoffControl", "talas.constant", {{"value", "2"}}));
    document.graph.connections.push_back(connection(
        "cutoffControl", "value", "avg", "cutoff"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {4.0, 8});

    require(
        result.ok(),
        "C139 connected cutoff should own validation over its inert invalid fallback: "
            + compileDiagnosticsSummary(result));
}

void testInvalidSampleParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "-0.1"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "0 s"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sample", "start");

    result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.8"}, {"loopEnd", "0.2"}, booleanParameter("loopFade", false), {"start", "0.0"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "0 s"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sample", "loopEnd");

    result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "0.0"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "-1 ms"}, {"releaseTime", "0 s"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sample", "attackTime");

    result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "0.0"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "-1 ms"}, {"scale", "1.0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sample", "releaseTime");

    result = talas::runtime::compileProject(
        makeSampleOutputDocument({{"frequency", "440 hz"}, {"gate", "1.0"}, resourceParameter("sample", "Samples/Hit"), booleanParameter("setLoop", false), {"loopStart", "0.0"}, {"loopEnd", "1.0"}, booleanParameter("loopFade", false), {"start", "0.0"}, {"end", "1.0"}, booleanParameter("fromMemoryWriter", false), {"attackTime", "0 s"}, {"releaseTime", "0 s"}, {"scale", "1 / 0"}}),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "sample", "scale");
}

void testInvalidDiskPlayerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeDiskPlayerOutputDocument(diskPlayerParameters({{"filePosition", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "filePosition");

    result = talas::runtime::compileProject(
        makeDiskPlayerOutputDocument(diskPlayerParameters({{"duration", "0 s"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "duration");

    result = talas::runtime::compileProject(
        makeDiskPlayerOutputDocument(diskPlayerParameters({{"rateScale", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "rateScale");

    result = talas::runtime::compileProject(
        makeDiskPlayerOutputDocument(diskPlayerParameters({{"trigger", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "trigger");
}

void testInvalidMultifileDiskPlayerParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeMultifileDiskPlayerOutputDocument(multifileDiskPlayerParameters({{"rateScale", "0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multiDisk", "rateScale");

    result = talas::runtime::compileProject(
        makeMultifileDiskPlayerOutputDocument(multifileDiskPlayerParameters({{"trigger", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multiDisk", "trigger");

    result = talas::runtime::compileProject(
        makeMultifileDiskPlayerOutputDocument(multifileDiskPlayerParameters({{"index", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multiDisk", "index");

}

void testInvalidSamplesFromDiskSingleStepParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeSamplesFromDiskSingleStepOutputDocument(samplesFromDiskSingleStepParameters({{"filePosition", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "filePosition");

    result = talas::runtime::compileProject(
        makeSamplesFromDiskSingleStepOutputDocument(samplesFromDiskSingleStepParameters({{"filePosition", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "filePosition");

    result = talas::runtime::compileProject(
        makeSamplesFromDiskSingleStepOutputDocument(samplesFromDiskSingleStepParameters({{"trigger", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "trigger");

    result = talas::runtime::compileProject(
        makeSamplesFromDiskSingleStepOutputDocument(samplesFromDiskSingleStepParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "disk", "gate");
}

void testInvalidMultisampleParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"frequency", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "frequency");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "gate");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"index", "-1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "index");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"index", "1.5"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "index");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"loopStart", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "loopStart");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"loopStart", "0.8"}, {"loopEnd", "0.2"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "loopEnd");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"attackTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "attackTime");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"releaseTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "releaseTime");

    result = talas::runtime::compileProject(
        makeMultisampleOutputDocument(multisampleParameters({{"scale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "multi", "scale");
}

void testInvalidKeyMappedMultisampleParametersFailCompile()
{
    auto result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"frequency", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "frequency");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"gate", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "gate");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"velocity", "-0.1"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "velocity");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"loFreq", "-1 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "loFreq");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"loFreq", "880 hz"}, {"hiFreq", "220 hz"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "hiFreq");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"attackTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "attackTime");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"releaseTime", "-1 ms"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "releaseTime");

    result = talas::runtime::compileProject(
        makeKeyMappedMultisampleOutputDocument(keyMappedMultisampleParameters({{"scale", "1 / 0"}})),
        makeRegistry(),
        {});
    requireCompileFailure(result, DiagnosticCode::ParameterEvaluationError, "keymap", "scale");
}

void testMissingAudioOutputFailsCompile()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("osc", "talas.oscillator"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::MissingAudioOutput);
}

void testMultipleAudioOutputsFailCompile()
{
    talas::project::ProjectDocument document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node("out2", "talas.audioOutput"));
    document.graph.connections.push_back(connection("vca", "audioOut", "out2", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::MultipleAudioOutputs);
}

void testDuplicateConnectionsIntoSameInputFailCompile()
{
    talas::project::ProjectDocument document;
    // A VCA's Inputs may be many; an Audio Output's audioIn is one.
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("osc1", "talas.oscillator"));
    document.graph.nodes.push_back(node("osc2", "talas.oscillator"));
    document.graph.connections.push_back(connection("osc1", "audioOut", "out", "audioIn"));
    document.graph.connections.push_back(connection("osc2", "audioOut", "out", "audioIn"));

    const auto result = talas::runtime::compileProject(document, makeRegistry(), {});

    requireCompileFailure(result, DiagnosticCode::DuplicateInputConnection);
    require(hasDiagnostic(result, DiagnosticCode::DuplicateInputConnection), "duplicate input diagnostic should be present");
}

void testUnsupportedReachableNodeTypeFailsCompile()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back(node("out", "talas.audioOutput"));
    document.graph.nodes.push_back(node("unsupported", "talas.unsupportedProbe"));
    document.graph.connections.push_back(connection("unsupported", "audioOut", "out", "audioIn"));

    auto registry = makeRegistry();
    registry.addNodeType({
        "talas.unsupportedProbe",
        "Unsupported Probe",
        "Tests",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {},
    });

    const auto result = talas::runtime::compileProject(document, registry, {});

    requireCompileFailure(result, DiagnosticCode::UnsupportedNodeType, "unsupported");
}

void testInvalidRenderSettingsFailCompile()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.maxBlockFrames = 0;

    const auto result = talas::runtime::compileProject(makeBasicAudioDocument(), makeRegistry(), settings);

    requireCompileFailure(result, DiagnosticCode::InvalidRenderSettings);

    settings = {};
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisQueueCapacity = 0;
    const auto invalidCapture = talas::runtime::compileProject(
        makeBasicAudioDocument(),
        makeRegistry(),
        settings);
    requireCompileFailure(invalidCapture, DiagnosticCode::InvalidRenderSettings);

    settings = {};
    settings.transportLoop =
        talas::runtime::TransportLoopRegion{8, 8};
    const auto invalidLoop = talas::runtime::compileProject(
        makeBasicAudioDocument(),
        makeRegistry(),
        settings);
    requireCompileFailure(invalidLoop, DiagnosticCode::InvalidRenderSettings);
}

void testCompilePlanCarriesOscillatorKernelBindings()
{
    const auto result = talas::runtime::compileProject(
        makeBasicAudioDocument(),
        makeRegistry(),
        {});
    require(result.ok(), "oscillator kernel-binding fixture should compile");

    const auto& steps = result.graph->plan().steps;
    const auto oscillator = std::ranges::find(
        steps,
        std::string{"talas.oscillator"},
        &talas::runtime::CompileStep::typeId);
    require(oscillator != steps.end(), "compile plan should contain oscillator step");
    require(
        oscillator->offlineSupport == talas::core::ExecutionSupport::Supported
            && oscillator->offlineBinding == talas::core::ExecutionBinding::Kernel
            && oscillator->preparedRealtimeSupport == talas::core::ExecutionSupport::Supported
            && oscillator->preparedRealtimeBinding == talas::core::ExecutionBinding::Kernel
            && oscillator->kernelKind == talas::runtime::KernelKind::Oscillator
            && oscillator->supportsReset
            && oscillator->resolvedLatencyFrames == 0
            && oscillator->inputLatencyFrames.size()
                == oscillator->inputs.size()
            && !oscillator->preparedIsolatedComponent,
        "C160 compile plan should preserve exact promoted Oscillator kernel, reset, latency, and input authority");

    auto contextualDocument = makeBasicAudioDocument(
        {{"amplitude", "@gain"}});
    contextualDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    contextualDocument.variableLibrary.variables.push_back({
        "gain",
        "Project gain",
        talas::expr::ValueDomain::Scalar,
        0.5,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    const auto contextual = talas::runtime::compileProject(
        contextualDocument,
        makeRegistry(),
        {});
    require(contextual.ok(),
        "C160 variable-expression Oscillator fixture should compile;"
            + compileDiagnosticsSummary(contextual));
    const auto contextualStep = std::ranges::find(
        contextual.graph->plan().steps,
        std::string{"talas.oscillator"},
        &talas::runtime::CompileStep::typeId);
    require(contextualStep != contextual.graph->plan().steps.end()
            && contextualStep->offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && contextualStep->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep->preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel,
        "C160 expression profiles outside exact authority should downgrade coherently to both-Subset");
}

void testCompilePlanCarriesLfoResetAndLatencyAuthority()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node(
        "lfo", "talas.lfo", {
            {"frequency", "1 hz"}, {"minimum", "0"},
            {"maximum", "1"},
        }));
    document.graph.connections.push_back(connection(
        "lfo", "value", "vca", "gain"));
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(), "C96 LFO authority fixture should compile");

    const auto& steps = result.graph->plan().steps;
    const auto lfo = std::ranges::find(
        steps, std::string{"talas.lfo"},
        &talas::runtime::CompileStep::typeId);
    require(lfo != steps.end(), "compile plan should contain LFO step");
    require(
        lfo->offlineSupport == talas::core::ExecutionSupport::Supported
            && lfo->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && lfo->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && lfo->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && !lfo->kernelKind.has_value()
            && lfo->supportsReset
            && lfo->resolvedLatencyFrames == 0
            && lfo->inputLatencyFrames.size() == lfo->inputs.size()
            && std::ranges::none_of(
                lfo->inputLatencyFrames,
                [](const auto latency) { return latency.has_value(); })
            && !lfo->preparedIsolatedComponent,
        "C182 compile plan should preserve exact promoted LFO legacy, reset, latency, and input authority");
}

void testConnectedLfoFrequencyOwnsCompileTimeValidation()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node(
        "lfoFrequency", "talas.constant", {{"value", "1"}}));
    document.graph.nodes.push_back(node(
        "lfo", "talas.lfo", {
            {"frequency", "-1 hz"}, {"minimum", "0"},
            {"maximum", "1"},
        }));
    document.graph.connections.push_back(connection(
        "lfoFrequency", "value", "lfo", "frequency"));
    document.graph.connections.push_back(connection(
        "lfo", "value", "vca", "gain"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(),
        "C96 connected LFO frequency should own compile-time validation over its inert negative fallback");
}

void testCompilePlanCarriesRampResetAndLatencyAuthority()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node(
        "ramp", "talas.ramp", {
            {"trigger", "1"}, {"duration", "1 s"},
            choiceParameter("variant", "ramp"),
        }));
    document.graph.connections.push_back(connection(
        "ramp", "value", "vca", "gain"));
    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(), "C97 Ramp authority fixture should compile");

    const auto& steps = result.graph->plan().steps;
    const auto ramp = std::ranges::find(
        steps, std::string{"talas.ramp"},
        &talas::runtime::CompileStep::typeId);
    require(ramp != steps.end(), "compile plan should contain Ramp step");
    require(
        ramp->offlineSupport == talas::core::ExecutionSupport::Supported
            && ramp->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ramp->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && ramp->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && !ramp->kernelKind.has_value()
            && ramp->supportsReset
            && ramp->resolvedLatencyFrames == 0
            && ramp->inputLatencyFrames.size() == ramp->inputs.size()
            && std::ranges::none_of(
                ramp->inputLatencyFrames,
                [](const auto latency) { return latency.has_value(); })
            && !ramp->preparedIsolatedComponent,
        "C184 compile plan should preserve exact Ramp Supported legacy, reset, latency, and input authority");
}

void testCompilePlanCarriesSampleAndHoldResetAndLatencyAuthority()
{
    const auto result = talas::runtime::compileProject(
        makeSampleAndHoldOutputDocument({
            {"holdTime", "0.5 s"}, {"offsetTime", "0 s"}}),
        makeRegistry(), {});
    require(result.ok(),
        "C98 Sample And Hold authority fixture should compile");

    const auto& steps = result.graph->plan().steps;
    const auto hold = std::ranges::find(
        steps, std::string{"talas.sampleAndHold"},
        &talas::runtime::CompileStep::typeId);
    require(hold != steps.end(),
        "compile plan should contain Sample And Hold step");
    require(
        hold->offlineSupport == talas::core::ExecutionSupport::Supported
            && hold->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && hold->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && hold->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && !hold->kernelKind.has_value()
            && hold->supportsReset
            && hold->resolvedLatencyFrames == 0
            && hold->inputLatencyFrames.size() == hold->inputs.size()
            && std::ranges::none_of(
                hold->inputLatencyFrames,
                [](const auto latency) { return latency.has_value(); })
            && !hold->preparedIsolatedComponent,
        "C185 compile plan should preserve exact Sample And Hold Supported legacy, reset, latency, and input authority");
}

void testConnectedSampleAndHoldTimeOwnsCompileTimeValidation()
{
    auto document = makeSampleAndHoldOutputDocument({
        {"holdTime", "1e200 * 1e200 s"}, {"offsetTime", "0 s"}});
    document.graph.nodes.push_back(node(
        "holdTime", "talas.constant", {{"value", "0.5"}}));
    document.graph.connections.push_back(connection(
        "holdTime", "value", "hold", "holdTime"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(),
        "C98 connected Sample And Hold time should own compile-time validation over its inert non-finite fallback");
}

void testConnectedRampDurationOwnsCompileTimeValidation()
{
    auto document = makeBasicAudioDocument();
    document.graph.nodes.push_back(node(
        "rampDuration", "talas.constant", { {"value", "1"} }));
    document.graph.nodes.push_back(node(
        "ramp", "talas.ramp", {
            {"trigger", "1"}, {"duration", "0 s"},
            choiceParameter("variant", "ramp"),
        }));
    document.graph.connections.push_back(connection(
        "rampDuration", "value", "ramp", "duration"));
    document.graph.connections.push_back(connection(
        "ramp", "value", "vca", "gain"));

    const auto result = talas::runtime::compileProject(
        document, makeRegistry(), {});
    require(result.ok(),
        "C97 connected Ramp duration should own compile-time validation over its inert zero fallback");
}

void testCompilePlanCarriesBoundedAnalogSequencerRealtimeAuthority()
{
    const auto result = talas::runtime::compileProject(
        makeAnalogSequencerOutputDocument(),
        makeRegistry(),
        {});
    require(result.ok(),
        "bounded Analog Sequencer authority fixture should compile");

    const auto& steps = result.graph->plan().steps;
    const auto sequencer = std::ranges::find(
        steps,
        std::string_view{"talas.analogSequencer"},
        &talas::runtime::CompileStep::typeId);
    require(sequencer != steps.end(),
        "compile plan should contain Analog Sequencer step");
    require(
        sequencer->offlineSupport == talas::core::ExecutionSupport::Supported
            && sequencer->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sequencer->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && sequencer->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sequencer->supportsReset
            && sequencer->resolvedLatencyFrames == 0
            && !sequencer->kernelKind.has_value()
            && !sequencer->preparedIsolatedComponent,
        "C162 compile plan should preserve exact promoted Analog Sequencer authority");
}

void testConnectedAnalogSequencerControlOwnsExactNonFiniteFallback()
{
    for (const auto [port, value] : {
             std::pair{std::string_view{"left"},
                 std::string_view{"0.5"}},
             std::pair{std::string_view{"right"},
                 std::string_view{"-0.5"}},
             std::pair{std::string_view{"gate"},
                 std::string_view{"1"}},
             std::pair{std::string_view{"step"},
                 std::string_view{"2"}},
             std::pair{std::string_view{"rate"},
                 std::string_view{"1"}}})
    {
        auto document = makeAnalogSequencerOutputDocument(
            analogSequencerParameters({{
                std::string{port}, "1e308 * 1e308"}}));
        const auto controlId = std::string{port} + "Control";
        document.graph.nodes.push_back(node(
            controlId, "talas.constant", {{"value", std::string{value}}}));
        document.graph.connections.push_back(connection(
            controlId, "value", "seq", std::string{port}));

        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(result.ok(),
            "C162 connected Analog Sequencer " + std::string{port}
                + " should own one exact inert non-finite fallback");
    }

    for (const auto invalidFallback : {
             std::string_view{"0"},
             std::string_view{"440 hz"},
             std::string_view{"!C162MissingRate"}})
    {
        auto document = makeAnalogSequencerOutputDocument(
            analogSequencerParameters({{
                "rate", std::string{invalidFallback}}}));
        document.graph.nodes.push_back(node(
            "rateControl", "talas.constant", {{"value", "1"}}));
        document.graph.connections.push_back(connection(
            "rateControl", "value", "seq", "rate"));
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(!result.ok(),
            "C162 connected rate should suppress only one exact non-finite numeric fallback");
    }
}

void testCompilePlanCarriesC15StereoRoutingRealtimeAuthority()
{
    const auto requireAuthority = [](
                                      const talas::project::ProjectDocument& document,
                                      std::string_view typeId,
                                      talas::core::ExecutionSupport preparedSupport) {
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(result.ok(),
                std::string{typeId} + " authority fixture should compile");
        const auto& steps = result.graph->plan().steps;
        const auto step = std::ranges::find(
            steps, typeId, &talas::runtime::CompileStep::typeId);
        require(step != steps.end(),
                std::string{typeId} + " compile step should exist");
        require(
            step->offlineSupport == talas::core::ExecutionSupport::Supported
                && step->offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->preparedRealtimeSupport
                    == preparedSupport
                && step->preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && !step->supportsReset
                && step->resolvedLatencyFrames == 0
                && !step->kernelKind.has_value()
                && !step->preparedIsolatedComponent,
            std::string{typeId}
                + " compile plan should preserve exact stateless C15/C130 authority");
    };

    requireAuthority(
        makeLevelOutputDocument({{"left", "0.25"}, {"right", "0.75"}}),
        "talas.level", talas::core::ExecutionSupport::Supported);
    requireAuthority(
        makeChannelCrosserOutputDocument(),
        "talas.channelCrosser", talas::core::ExecutionSupport::Supported);
    requireAuthority(
        makeChannelJoinOutputDocument(),
        "talas.channelJoin", talas::core::ExecutionSupport::Supported);
    requireAuthority(
        makeChannellerOutputDocument({
            booleanParameter("leftChannel", true),
            booleanParameter("rightChannel", false),
        }),
        "talas.channeller", talas::core::ExecutionSupport::Supported);
}

void testCompilePlanCarriesC192ShelvingPresenceRealtimeAuthority()
{
    const auto requireAuthority = [](
                                      const talas::project::ProjectDocument& document,
                                      std::string_view typeId) {
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(result.ok(),
                std::string{typeId} + " authority fixture should compile");
        const auto& steps = result.graph->plan().steps;
        const auto step = std::ranges::find(
            steps, typeId, &talas::runtime::CompileStep::typeId);
        require(step != steps.end(),
                std::string{typeId} + " compile step should exist");
        require(
            step->offlineSupport == talas::core::ExecutionSupport::Supported
                && step->offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported
                && step->preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->supportsReset
                && step->resolvedLatencyFrames == 0
                && !step->kernelKind.has_value()
                && !step->preparedIsolatedComponent,
            std::string{typeId}
                + " compile plan should preserve exact stateful C192 authority");
    };

    requireAuthority(
        makeShelvingFilterOutputDocument(
            "highShelf", "talas.highShelvingFilter"),
        "talas.highShelvingFilter");
    requireAuthority(
        makeShelvingFilterOutputDocument(
            "lowShelf", "talas.lowShelvingFilter"),
        "talas.lowShelvingFilter");
    requireAuthority(
        makePresenceFilterOutputDocument(),
        "talas.presenceFilter");
}

void testCompilePlanCarriesC17CheckedBiquadRealtimeAuthority()
{
    const auto requireAuthority = [](
                                      const talas::project::ProjectDocument& document,
                                      std::string_view typeId) {
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(result.ok(),
                std::string{typeId} + " C17 authority fixture should compile");
        const auto& steps = result.graph->plan().steps;
        const auto step = std::ranges::find(
            steps, typeId, &talas::runtime::CompileStep::typeId);
        require(step != steps.end(),
                std::string{typeId} + " compile step should exist");
        require(
            step->offlineSupport == talas::core::ExecutionSupport::Subset
                && step->offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported
                && step->preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->supportsReset
                && step->resolvedLatencyFrames == 0
                && !step->kernelKind.has_value()
                && !step->preparedIsolatedComponent,
            std::string{typeId}
                + " compile plan should preserve exact stateful C193 authority");
    };

    requireAuthority(
        makeTwoFormantElementOutputDocument(),
        "talas.twoFormantElement");
    requireAuthority(
        makeTwoFormantVoiceElementOutputDocument(),
        "talas.twoFormantVoiceElement");
    requireAuthority(
        makeDualParallelTwoPoleFilterOutputDocument(),
        "talas.dualParallelTwoPoleFilter");
    requireAuthority(
        makeGraphicEqOutputDocument(),
        "talas.graphicEq");

    const auto requireContextualDemotion = [](
        talas::project::ProjectDocument document,
        std::string_view nodeId,
        std::string_view parameterId,
        std::string expression,
        std::string_view typeId) {
        document.expressionLanguage =
            talas::expr::ExpressionLanguageVersion::VariablesV2;
        const auto graphNode = std::ranges::find(
            document.graph.nodes,
            nodeId,
            &talas::project::ProjectNode::instanceId);
        require(
            graphNode != document.graph.nodes.end(),
            std::string{typeId}
                + " contextual fixture should expose its graph node");
        const auto parameter = std::ranges::find(
            graphNode->parameters,
            parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        require(
            parameter != graphNode->parameters.end(),
            std::string{typeId}
                + " contextual fixture should expose its parameter");
        parameter->expression = std::move(expression);
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), {});
        require(
            result.ok(),
            std::string{typeId}
                + " contextual stateful expression should compile");
        const auto step = std::ranges::find(
            result.graph->plan().steps,
            typeId,
            &talas::runtime::CompileStep::typeId);
        require(
            step != result.graph->plan().steps.end()
                && step->offlineSupport
                    == talas::core::ExecutionSupport::Subset
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Subset,
            std::string{typeId}
                + " unsafe expression must remain contextual both-Subset");

        const auto offline = talas::runtime::renderOffline(*result.graph, 4);
        talas::runtime::RealtimeRenderSession prepared{*result.graph};
        std::vector<double> actual(4);
        require(
            offline.ok() && prepared.ready()
                && prepared.render(actual)
                && actual == offline.mono,
            std::string{typeId}
                + " compiler-demoted both-Subset graph must remain executable: "
                + std::string{prepared.error()});
    };
    requireContextualDemotion(
        makeTwoFormantElementOutputDocument(),
        "formants", "formant1",
        "previous(1000 hz, 1000 hz)",
        "talas.twoFormantElement");
    requireContextualDemotion(
        makeTwoFormantVoiceElementOutputDocument(),
        "voice", "frequency",
        "previous(220 hz, 220 hz)",
        "talas.twoFormantVoiceElement");
    requireContextualDemotion(
        makeDualParallelTwoPoleFilterOutputDocument(),
        "twoPole", "scale1", "previous(1, 1)",
        "talas.dualParallelTwoPoleFilter");
    requireContextualDemotion(
        makeGraphicEqOutputDocument(),
        "eq", "cf250Hz", "previous(1, 1)",
        "talas.graphicEq");
    requireContextualDemotion(
        makeTwoFormantElementOutputDocument(),
        "formants", "formant1", "100 hz * [pi, 1][1]",
        "talas.twoFormantElement");

    auto variableDocument = makeGraphicEqOutputDocument();
    variableDocument.variableLibrary.variables.push_back({
        "bandLevel",
        "Band Level",
        talas::expr::ValueDomain::Scalar,
        1.0,
        {talas::project::ProjectVariableOwnerKind::Project, {}},
        true,
    });
    requireContextualDemotion(
        std::move(variableDocument),
        "eq", "cf250Hz", "@bandLevel",
        "talas.graphicEq");

    const auto analysisResult = talas::runtime::compileProject(
        makeAnalysisFilterOutputDocument(), makeRegistry(), {});
    require(analysisResult.ok(),
            "C164 Analysis Filter authority fixture should compile");
    const auto& analysis = requireStep(
        analysisResult.graph->plan(), "analysis");
    require(
        analysis.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && analysis.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && analysis.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && analysis.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && analysis.supportsReset
            && analysis.resolvedLatencyFrames == 0
            && !analysis.kernelKind.has_value()
            && !analysis.preparedIsolatedComponent,
        "C164 Analysis Filter compile plan should preserve exact promoted authority");

    auto contextualDocument = makeAnalysisFilterOutputDocument();
    const auto analysisNode = std::ranges::find(
        contextualDocument.graph.nodes,
        std::string_view{"analysis"},
        &talas::project::ProjectNode::instanceId);
    require(analysisNode != contextualDocument.graph.nodes.end(),
            "C164 contextual fixture should expose Analysis Filter");
    const auto fundamental = std::ranges::find(
        analysisNode->parameters,
        std::string_view{"fundamental"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(fundamental != analysisNode->parameters.end(),
            "C164 contextual fixture should expose fundamental");
    fundamental->expression = "50 hz + 50 hz";
    const auto contextual = talas::runtime::compileProject(
        contextualDocument, makeRegistry(), {});
    require(contextual.ok(),
            "C164 contextual Analysis Filter fixture should compile");
    const auto& contextualStep = requireStep(
        contextual.graph->plan(), "analysis");
    require(
        contextualStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C164 compound Analysis Filter expressions should remain contextual both-Subset without promoted authority");
}

void testPhaseShiftBy90CompilePlanCarriesScheduledPromotedAuthority()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {
        {"C165Frequency", 440.0, talas::expr::ValueDomain::Hertz},
    };
    const auto scheduled = talas::runtime::compileProject(
        makePhaseShiftBy90OutputDocument({
            {"frequency", "!C165Frequency"},
        }),
        makeRegistry(),
        settings);
    require(scheduled.ok(),
        "C165 direct scheduled Phase Shift By 90 fixture should compile");
    const auto& promoted = requireStep(scheduled.graph->plan(), "phase");
    require(
        promoted.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && promoted.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && promoted.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && promoted.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && promoted.supportsReset
            && promoted.resolvedLatencyFrames == 0
            && promoted.cumulativeOutputLatencyFrames == 0
            && !promoted.kernelKind.has_value()
            && !promoted.preparedIsolatedComponent
            && promoted.parameters.size() == 2
            && promoted.parameters[0].id == "frequency"
            && promoted.parameters[1].id == "overflow"
            && promoted.parameters[0].domain
                == talas::expr::ValueDomain::Hertz
            && promoted.parameters[0].expression.has_value()
            && promoted.parameters[0].expression->instructionCount() == 1
            && promoted.parameters[0].expression->eventReferences()
                == std::vector<std::string>{"C165Frequency"},
        "C165 compile plan should retain exact promoted direct-frequency authority");

    const auto contextual = talas::runtime::compileProject(
        makePhaseShiftBy90OutputDocument({
            {"frequency", "220 hz + 220 hz"},
        }),
        makeRegistry(),
        {});
    require(contextual.ok(),
        "C165 contextual compound frequency fixture should compile");
    const auto& contextualStep = requireStep(
        contextual.graph->plan(), "phase");
    require(
        contextualStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C165 compound frequency should be explicitly demoted to both-Subset");

    auto connectedDocument = makePhaseShiftBy90OutputDocument({
        {"frequency", "1e200 * 1e200"},
    });
    connectedDocument.graph.nodes.push_back(node(
        "frequencyControl", "talas.constant", {{"value", "440"}}));
    connectedDocument.graph.connections.push_back(connection(
        "frequencyControl", "value", "phase", "frequency"));
    const auto connected = talas::runtime::compileProject(
        connectedDocument, makeRegistry(), {});
    require(connected.ok(),
        "C165 connected precedence should suppress one exact NonFiniteResult fallback");
    const auto& connectedStep = requireStep(
        connected.graph->plan(), "phase");
    require(
        connectedStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && connectedStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C165 suppressed compound fallback should remain contextual both-Subset");

    auto unresolvedDocument = connectedDocument;
    const auto phase = std::ranges::find(
        unresolvedDocument.graph.nodes,
        std::string_view{"phase"},
        &talas::project::ProjectNode::instanceId);
    require(phase != unresolvedDocument.graph.nodes.end(),
        "C165 unresolved fallback fixture should expose Phase Shift By 90");
    phase->parameters = {{"frequency", "!C165MissingFrequency"}};
    const auto unresolved = talas::runtime::compileProject(
        unresolvedDocument, makeRegistry(), {});
    require(!unresolved.ok(),
        "C165 connected precedence must not hide an unresolved fallback");
}

void testSingleSideBandRmCompilePlanCarriesDualScheduledPromotedAuthority()
{
    auto settings = talas::runtime::RenderSettings{};
    settings.eventValues = {
        {"C166Frequency", 440.0, talas::expr::ValueDomain::Hertz},
        {"C166FreqScale", 2.0, talas::expr::ValueDomain::Scalar},
    };
    const auto scheduled = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({
            {"frequency", "!C166Frequency"},
            {"freqScale", "!C166FreqScale"},
        }),
        makeRegistry(),
        settings);
    require(scheduled.ok(),
        "C166 direct dual-scheduled Single Side Band RM fixture should compile");
    const auto& promoted = requireStep(scheduled.graph->plan(), "ssb");
    require(
        promoted.offlineSupport
                == talas::core::ExecutionSupport::Supported
            && promoted.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && promoted.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Supported
            && promoted.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && promoted.supportsReset
            && promoted.resolvedLatencyFrames == 0
            && promoted.cumulativeOutputLatencyFrames == 0
            && !promoted.kernelKind.has_value()
            && !promoted.preparedIsolatedComponent
            && promoted.parameters.size() == 2
            && promoted.parameters[0].id == "frequency"
            && promoted.parameters[0].domain
                == talas::expr::ValueDomain::Hertz
            && promoted.parameters[0].expression.has_value()
            && promoted.parameters[0].expression->instructionCount() == 1
            && promoted.parameters[0].expression->eventReferences()
                == std::vector<std::string>{"C166Frequency"}
            && promoted.parameters[1].id == "freqScale"
            && promoted.parameters[1].domain
                == talas::expr::ValueDomain::Scalar
            && promoted.parameters[1].expression.has_value()
            && promoted.parameters[1].expression->instructionCount() == 1
            && promoted.parameters[1].expression->eventReferences()
                == std::vector<std::string>{"C166FreqScale"},
        "C166 compile plan should retain exact promoted dual-schedule authority");

    const auto contextual = talas::runtime::compileProject(
        makeSingleSideBandRmOutputDocument({
            {"frequency", "220 hz + 220 hz"},
            {"freqScale", "1 + 1"},
        }),
        makeRegistry(),
        {});
    require(contextual.ok(),
        "C166 contextual compound-control fixture should compile");
    const auto& contextualStep = requireStep(
        contextual.graph->plan(), "ssb");
    require(
        contextualStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && contextualStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C166 compound controls should be explicitly demoted to both-Subset");

    auto connectedDocument = makeSingleSideBandRmOutputDocument({
        {"frequency", "1e200 * 1e200"},
        {"freqScale", "1e200 * 1e200"},
    });
    connectedDocument.graph.nodes.push_back(node(
        "frequencyControl", "talas.constant", {{"value", "440"}}));
    connectedDocument.graph.nodes.push_back(node(
        "scaleControl", "talas.constant", {{"value", "2"}}));
    connectedDocument.graph.connections.push_back(connection(
        "frequencyControl", "value", "ssb", "frequency"));
    connectedDocument.graph.connections.push_back(connection(
        "scaleControl", "value", "ssb", "freqScale"));
    const auto connected = talas::runtime::compileProject(
        connectedDocument, makeRegistry(), {});
    require(connected.ok(),
        "C166 connected precedence should suppress each exact NonFiniteResult fallback");
    const auto& connectedStep = requireStep(
        connected.graph->plan(), "ssb");
    require(
        connectedStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && connectedStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C166 suppressed compound fallbacks should remain contextual both-Subset");

    auto legacyConnectedDocument = connectedDocument;
    const auto legacySsb = std::ranges::find(
        legacyConnectedDocument.graph.nodes,
        std::string_view{"ssb"},
        &talas::project::ProjectNode::instanceId);
    require(legacySsb != legacyConnectedDocument.graph.nodes.end(),
        "C166 legacy compatibility fixture should expose Single Side Band RM");
    for (auto& parameter : legacySsb->parameters)
        parameter.expression = "1 / 0";
    const auto legacyConnected = talas::runtime::compileProject(
        legacyConnectedDocument, makeRegistry(), {});
    require(legacyConnected.ok(),
        "C166 compiler-demoted connected subset should preserve C101 DivisionByZero fallback compatibility");
    const auto& legacyStep = requireStep(
        legacyConnected.graph->plan(), "ssb");
    require(
        legacyStep.offlineSupport
                == talas::core::ExecutionSupport::Subset
            && legacyStep.preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset,
        "C166 legacy connected fallbacks must not gain promoted authority");
}

void testCompilePlanCarriesC18StaticFirRealtimeAuthority()
{
    const auto requireAuthority = [](
                                      const talas::project::ProjectDocument& document,
                                      std::string_view typeId,
                                      talas::runtime::RenderSettings settings = {}) {
        const auto result = talas::runtime::compileProject(
            document, makeRegistry(), std::move(settings));
        require(
            result.ok(),
            std::string{typeId} + " C18 authority fixture should compile");
        const auto& steps = result.graph->plan().steps;
        const auto step = std::ranges::find(
            steps, typeId, &talas::runtime::CompileStep::typeId);
        require(
            step != steps.end(),
            std::string{typeId} + " compile step should exist");
        require(
            step->offlineSupport == talas::core::ExecutionSupport::Subset
                && step->offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->preparedRealtimeSupport
                    == talas::core::ExecutionSupport::Supported
                && step->preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && step->supportsReset
                && step->resolvedLatencyFrames == 0
                && !step->kernelKind.has_value()
                && !step->preparedIsolatedComponent,
            std::string{typeId}
                + " compile plan should publish exact stateful C194 authority");
    };

    requireAuthority(makeFirFilterOutputDocument(), "talas.firFilter");
    auto reSettings = talas::runtime::RenderSettings{};
    reSettings.tableResources.push_back({
        "Tables/RE", {0.5, 0.25, -0.125}});
    requireAuthority(
        makeReResonatorOutputDocument(),
        "talas.reResonator",
        std::move(reSettings));
}

void testCompilePlanCarriesC19VcfRealtimeAuthority()
{
    const auto result = talas::runtime::compileProject(
        makeVcfOutputDocument(), makeRegistry(), {});
    require(result.ok(), "C19 VCF authority fixture should compile");
    const auto& steps = result.graph->plan().steps;
    const auto step = std::ranges::find(
        steps,
        std::string_view{"talas.vcf"},
        &talas::runtime::CompileStep::typeId);
    require(step != steps.end(), "compile plan should contain C19 VCF");
    require(
        step->offlineSupport == talas::core::ExecutionSupport::Subset
            && step->offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step->preparedRealtimeSupport
                == talas::core::ExecutionSupport::Subset
            && step->preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step->supportsReset
            && step->resolvedLatencyFrames == 0
            && !step->kernelKind.has_value()
            && !step->preparedIsolatedComponent,
        "compile plan should preserve exact stateful C19 VCF authority");
}
}

int main()
{
    testEventValueDelayWithFeedbackRendersAcrossChanges();
    testLongChainsCompileOnAWorkerStack();
    testMissingComponentFailsBeforeResourceMaterialization();
    testNestedMissingComponentReportsQualifiedScopeBeforeResources();
    testIncompatibleComponentReportsExactReasonBeforeResources();
    testCompilePlanCarriesOscillatorKernelBindings();
    testCompilePlanCarriesLfoResetAndLatencyAuthority();
    testConnectedLfoFrequencyOwnsCompileTimeValidation();
    testCompilePlanCarriesRampResetAndLatencyAuthority();
    testCompilePlanCarriesSampleAndHoldResetAndLatencyAuthority();
    testConnectedSampleAndHoldTimeOwnsCompileTimeValidation();
    testConnectedRampDurationOwnsCompileTimeValidation();
    testCompilePlanCarriesBoundedAnalogSequencerRealtimeAuthority();
    testConnectedAnalogSequencerControlOwnsExactNonFiniteFallback();
    testCompilePlanCarriesC15StereoRoutingRealtimeAuthority();
    testCompilePlanCarriesC192ShelvingPresenceRealtimeAuthority();
    testCompilePlanCarriesC17CheckedBiquadRealtimeAuthority();
    testPhaseShiftBy90CompilePlanCarriesScheduledPromotedAuthority();
    testSingleSideBandRmCompilePlanCarriesDualScheduledPromotedAuthority();
    testCompilePlanCarriesC18StaticFirRealtimeAuthority();
    testCompilePlanCarriesC19VcfRealtimeAuthority();
    testScrambledDocumentCompilesIntoTopologicalPlanAndReachableBuffers();
    testParkedUnsupportedNodeDoesNotFailCompile();
    testParkedInvalidNodeAndConnectionDoNotFailCompile();
    testOmittedProjectParametersUseBuiltinDefaults();
    testProjectParameterOverridesWinOverDefaults();
    testInitialEventValueBindingsResolveTypedExpressions();
    testInitialEventValueBindingsRejectUnknownAndInvalidSettings();
    testRawMidiEventValueSchedulesCompileAndValidate();
    testConstantCompilePlanRetainsEventValueExpression();
    testConstantParametersNeedLessComputationThanTimeVaryingOnes();
    testChoiceParameterDefaultCompilesIntoRuntimePlan();
    testChoiceParameterOverrideCompilesIntoRuntimePlan();
    testInvalidChoiceParameterFailsCompile();
    testExpressionOverrideForChoiceParameterFailsCompile();
    testExpressionAndChoiceOverrideFailsCompile();
    testChoiceOverrideForExpressionParameterFailsCompile();
    testBooleanParameterDefaultCompilesIntoRuntimePlan();
    testBooleanParameterOverrideCompilesIntoRuntimePlan();
    testExpressionOverrideForBooleanParameterFailsCompile();
    testExpressionAndBooleanOverrideFailsCompile();
    testEmptyExpressionAndBooleanOverrideFailsCompile();
    testChoiceAndBooleanOverrideFailsCompile();
    testBooleanOverrideForExpressionParameterFailsCompile();
    testBooleanOverrideForChoiceParameterFailsCompile();
    testResourceTextParameterDefaultsCompileIntoRuntimePlan();
    testResourceTextParameterOverridesCompileIntoRuntimePlan();
    testExpressionOverrideForResourceParameterFailsCompile();
    testExpressionAndResourceOverrideFailsCompile();
    testBooleanAndTextOverrideFailsCompile();
    testResourceOverrideForExpressionParameterFailsCompile();
    testUnknownParameterOverrideFailsWithNodeAndParameter();
    testDuplicateParameterOverrideFailsWithNodeAndParameter();
    testInvalidParameterExpressionFailsWithoutDefaultFallback();
    testParameterEvaluationErrorFailsWithoutDefaultFallback();
    testNegativeOscillatorFrequencyCompiles();
    testInvalidReachableRampDurationFailsCompile();
    testNegativeReachableDelayFailsCompile();
    testConnectedDelayOwnsNegativeStaticFallback();
    testInvalidAveragingLowPassCutoffFailsCompile();
    testStereoAudioOutputCapturesRightBufferIndex();
    testOutput4GraphCompilesWithFourOutputBufferIndices();
    testOutput6GraphCompilesWithSixOutputBufferIndices();
    testSixOutputSoundPreparesExactTerminal();
    testOutput8GraphCompilesWithEightOutputBufferIndices();
    testStereoInOutput4GraphCompilesWithDuplicatedPairBufferIndices();
    testStereoInOutput8GraphCompilesWithDuplicatedPairBufferIndices();
    testMatrix4GraphCompilesWithFourOutputBufferIndices();
    testMatrix8GraphCompilesWithEightOutputBufferIndices();
    testForcedProcessorAssignmentGraphCompilesAsAudioPassthrough();
    testNestedForcedProcessorAssignmentPublishesBoundariesAndTransfers();
    testProcessorPlacementIsStableAcrossInsertionPermutations();
    testConflictingForcedProcessorAssignmentClosureFailsCompile();
    testForcedProcessorAssignmentProcessorCompilesToItsStartingValue();
    testProcessorPlacementUsesExpandedNestedSoundIdentity();
    testProcessorPlacementUsesExpandedMidiVoiceIdentities();
    testProcessorPlacementUsesScoreSoundPreparationIdentity();
    testPanGraphCompilesWithStereoOutputBuffers();
    testLevelGraphCompilesWithStereoOutputBuffers();
    testCrossfadeGraphCompilesWithStereoOutputBuffers();
    testDynamicRangeControllerGraphCompilesWithAudioOutputBuffer();
    testReverbSectionGraphCompilesWithAudioOutputBuffer();
    testChannelCrosserGraphCompilesWithStereoInputAndOutputBuffers();
    testChannelJoinGraphCompilesWithStereoOutputBuffers();
    testChannellerGraphCompilesWithStereoOutputBuffers();
    testChannellerGraphCompilesWithIndependentLiveSelectors();
    testFilterGraphCompilesWithAudioOutputBuffer();
    testFirFilterGraphCompilesWithAudioOutputBuffer();
    testShelvingFilterGraphsCompileWithAudioOutputBuffers();
    testPresenceFilterGraphCompilesWithAudioOutputBuffer();
    testTwoFormantElementGraphCompilesWithAudioOutputBuffer();
    testTwoFormantVoiceElementGraphCompilesWithAudioOutputBuffer();
    testDualParallelTwoPoleFilterGraphCompilesWithAudioOutputBuffer();
    testHarmonicResonatorGraphCompilesWithAudioOutputBuffer();
    testReResonatorGraphCompilesWithAudioOutputBuffer();
    testGraphicEqGraphCompilesWithAudioOutputBuffer();
    testVcfGraphCompilesWithAudioOutputBuffer();
    testSingleSideBandRmGraphCompilesWithAudioOutputBuffer();
    testPhaseShiftBy90GraphCompilesWithAudioOutputBuffer();
    testAnalysisFilterGraphCompilesWithStereoOutputBuffers();
    testAudioInputGraphCompilesWithAudioOutputBuffer();
    testGenericSourceGraphCompilesWithStereoOutputBuffers();
    testBlankMidiInGraphsCompileWithStereoOutputBuffers();
    testMappedMidiMapperCompilesExactPreparedPlan();
    testMappedMidiMapperAggregateLimitsFailClosed();
    testMidiVoiceCompilesIndependentPreparedVoiceScopes();
    testAdsrMixedExactAndPerVoiceSubsetAuthority();
    testMidiVoiceRejectsSharedIslandsAndLowersMultipleDisjointRegions();
    testNestedKeyOwnerCompilePlansPreserveParentVoiceScopes();
    testNestedMidiVoiceDeclaredCellsAndStepBudget();
    testNestedVoicePlansBindProjectedDescendantSoundDefaults();
    testMidiVoiceVariableCapacityFailsBeforePublication();
    testMidiVoiceConfigurationLimitsAreEnforced();
    testNoAudioTerminalGraphsCompileAsNoopOutputs();
    testStateSinkTerminalGraphsCompileAsNoopOutputs();
    testQuadOscillatorGraphCompilesWithAudioOutputBuffer();
    testThresholdCompilePlanPublishesStateAndResetAuthority();
    testSumOfSinesGraphCompilesWithAudioOutputBuffer();
    testC61SumOfSinesCompilesWithDefaultAnalysisFrequencies();
    testC62SumOfSinesCompilesWithIndependentAnalysisFrames();
    testC63SumOfSinesCompilesAutomaticGateLoopTraversal();
    testSumOfSinesMorphParametersCompile();
    testGaOscillatorsGraphCompilesWithAudioOutputBuffer();
    testGaOscillatorsMorphParametersCompile();
    testOscillatorBankGraphCompilesWithSpectrumInputAndAudioOutput();
    testSosOscillatorsGraphCompilesWithSpectrumCascadeAndAudioOutput();
    testSosOscillatorsRejectsNonintegralSelectionAndDynamicSpectrumAuthority();
    testLowPassGateCompileAuthorityAndConfigurationBounds();
    testFilterBankGraphCompilesWithSpectrumAndAudioInput();
    testFilterBankElementGraphCompilesWithSpectrumAudioAndCascadeInput();
    testFormantBankGraphCompilesWithSpectrumInput();
    testFormantBankNamedImpulseResourcesAndOverlapsCompile();
    testFormantBankOscillatorGraphCompilesWithSpectrumCascadeAndAudioOutput();
    testFormantBanksCompileWithMatchedSerializedPairInputs();
    testFormantBankOscillatorGraphCompilesWithDcFormantEnabled();
    testVocoderGraphsCompileAsRuntimeSubsets();
    testVocoderChannelBankGraphCompilesAsActiveMappedBank();
    testContextFreeGrammarGraphCompilesAsBlankSilenceSource();
    testLimeInterpreterGraphCompilesAsBlankStereoSource();
    testFeedbackLoopOutputGraphCompilesAsBlankSilenceSource();
    testNamedFeedbackLoopPairCompilesIntoExplicitDelayedComponent();
    testNamedFeedbackLoopPairValidationRejectsMissingAndDuplicateEndpoints();
    testFeedbackLoopRejectsOrdinaryCyclesAndDelayBudgetOverflow();
    testFeedbackLoopCompileOrderIsStableAcrossInsertionPermutations();
    testForcedProcessorAssignmentCannotSplitFeedbackComponent();
    testScriptGraphCompilesWithStereoOutputBuffers();
    testScriptGraphCompilesWithoutOptionalAudioInput();
    testParameterTransformerGraphCompilesAsBlankPassthrough();
    testParameterTransformerRewritesUpstreamParametersBeforeCompile();
    testPackedFftGraphCompilesWithExactLatencyAndMatrix();
    testPackedFftEventLengthRequiresReprepare();
    testNativeComplexStftGraphCompilesWithExactLatencyAndMatrix();
    testNativeComplexStftRejectsInvalidAndMismatchedPreparedConfiguration();
    testNativeComplexStftPreparedEventValuesRequireReprepare();
    testParallelNativeForwardNodesReceiveDistinctComplexBuffers();
    testSyntheticSpectrumTrackSelectorGraphCompilesWithSpectrumBuffer();
    testSyntheticSpectrumFromSoundsGraphCompilesWithSpectrumBuffer();
    testSyntheticSpectrumFromSoundsBandwidthsCompile();
    testSyntheticSpectrumFromSoundsLogScaleModesCompile();
    testLiveSpectralAnalysisGraphCompilesWithSpectrumBuffer();
    testLiveSpectralAnalysisDerivesExactPreparedFrameBoundaries();
    testLiveSpectralAnalysisCarriesCumulativeLatencyAndReprepareAuthority();
    testSpectrumInRamGraphCompilesWithSpectrumBuffer();
    testC59SpectrumInRamCompilesWithDefaultPitchAndPartialCount();
    testSpectrumInRamGraphCompilesWithStaticTimeIndex();
    testSpectrumInRamGraphCompilesWithFractionalTimeIndex();
    testSpectrumOnDiskGraphCompilesWithSpectrumBuffer();
    testSpectrumOnDiskHistoricalParametersUseC56Defaults();
    testC60SpectrumOnDiskCompilesWithDefaultPitchAndPartialCount();
    testSpectrumFrequencyScaleGraphCompilesWithSpectrumBuffer();
    testSpectrumLogToLinearGraphCompilesWithSpectrumOutput();
    testSpectrumFundamentalGraphCompilesWithStereoOutputBuffers();
    testSpectrumVoicedUnvoicedGraphCompilesWithStereoOutputBuffers();
    testSpectralShapeGraphCompilesWithSpectrumBuffer();
    testSpectrumModifierGraphCompilesWithSpectrumBuffer();
    testSpectrumModifierGraphCompilesWithIndependentLiveSelectors();
    testSpectrumModifierGraphCompilesWithLiveSeed();
    testSpectrumModifierProbabilityAndSeedCompile();
    testSpectrumModifierTrackNumberProbabilityProfileCompiles();
    testSpectrumModifierVariablesV2RemainderProbabilityProfileCompiles();
    testSpectrumModifierFrequencyHysteresisCompiles();
    testSpectrumModifierAmplitudeHysteresisCompiles();
    testSpectrumModifierRejectModeCompiles();
    testSampleGraphCompilesWithAudioOutputBuffer();
    testSampleGraphCompilesWithReversePlaybackRange();
    testSampleGraphCompilesWithNegativeFrequency();
    testDiskPlayerGraphCompilesWithAudioOutputBuffer();
    testDiskCacheGraphCompilesWithAudioOutputBuffer();
    testDiskRecorderGraphCompilesAsTerminalCapture();
    testDiskRecorderWritableProfileCompilesCanonicalAuthority();
    testDiskRecorderRecordsInEveryTalasFormat();
    testInvalidDiskRecorderParametersFailCompile();
    testMemoryWriterGraphCompilesWithAudioOutputBuffer();
    testSampleCloudGraphCompilesWithAudioOutputBuffer();
    testInvalidSampleCloudParametersFailCompile();
    testGrainCloudGraphCompilesWithStereoOutputBuffers();
    testGrainCloudGraphCompilesWithCycleDerivedDuration();
    testInvalidGrainCloudParametersFailCompile();
    testCloudBankGraphCompilesWithStereoOutputBuffers();
    testCloudBankGraphCompilesWithZeroCyclesPerGrain();
    testCloudBankGraphCompilesWithCycleDerivedDuration();
    testCloudBankGraphCompilesWithNearestInterpolation();
    testCloudBankElementGraphCompilesWithCascadeAndStereoOutputs();
    testCloudBankElementGraphCompilesWithZeroCyclesPerGrain();
    testCloudBankElementGraphCompilesWithCycleDerivedDuration();
    testCloudBankElementGraphCompilesWithNearestInterpolation();
    testInvalidCloudBankParametersFailCompile();
    testInvalidCloudBankElementParametersFailCompile();
    testMultifileDiskPlayerGraphCompilesWithAudioOutputBuffer();
    testMultifileDiskPlayerNegativeIndexCompilesForRuntimeClamping();
    testMultifileDiskPlayerGatedCompilesForRuntimeGateControl();
    testConnectedMultifileDiskPlayerControlsOwnUnevaluableFallbacks();
    testSamplesFromDiskSingleStepGraphCompilesWithAudioOutputBuffer();
    testMultisampleGraphCompilesWithAudioOutputBuffer();
    testMultisampleGraphCompilesWithNegativeFrequency();
    testKeyMappedMultisampleGraphCompilesWithAudioOutputBuffer();
    testAveragingLowPassFilterGraphCompilesWithAudioOutputBuffer();
    testDelayWithFeedbackGraphCompilesWithBoundedAuthority();
    testNoiseGraphCompilesWithAudioOutputBuffer();
    testInputOutputCharacteristicGraphCompilesWithAudioOutputBuffer();
    testWaveshaperGraphCompilesWithPolynomialOutputBuffer();
    testMultiplyingWaveshaperGraphCompilesWithAudioOutputBuffer();
    testIteratedWaveshaperGraphCompilesWithAudioOutputBuffer();
    testSampleAndHoldGraphCompilesWithControlOutputBuffer();
    testTalasRampGraphCompilesWithHotControlBindings();
    testPulseTrainGraphCompilesWithControlOutputBuffer();
    testPulseTrainIgnoresDutyCycleWhenVariableDutyCycleIsDisabled();
    testPulseGeneratorGraphCompilesWithAudioOutputBuffer();
    testMultislopeFunctionGeneratorGraphCompilesWithControlOutputBuffer();
    testMultisegmentEnvelopeGraphCompilesWithControlOutputBuffer();
    testGraphicalEnvelopeGraphCompilesWithControlOutputBuffer();
    testInvalidGraphicalEnvelopeParametersFailCompile();
    testFunctionGeneratorGraphCompilesWithControlOutputBuffer();
    testChopperGraphCompilesWithAudioOutputBuffer();
    testArGraphCompilesWithControlOutputBuffer();
    testAdsrGraphCompilesWithControlOutputBuffer();
    testTriggeredSampleAndHoldGraphCompilesWithControlOutputBuffer();
    testTriggeredTableReadGraphCompilesWithControlOutputBuffer();
    testFrequencyTrackerGraphCompilesWithControlOutputBuffer();
    testTimeGateGraphsCompileWithAudioOutputBuffers();
    testTimeControlGraphCompilesWithAudioOutputBuffer();
    testTimeOffsetGraphCompilesWithRetrogradeEnabled();
    testWarpedTimeIndexGraphCompilesWithControlOutputBuffer();
    testFrequencyScaleGraphCompilesWithAudioOutputBuffer();
    testTimeFrequencyScaleGraphCompilesWithAudioOutputBuffer();
    testTimeFrequencyScaleGraphCompilesWithFastRate();
    testMonotonizerGraphCompilesAsNeutralPitchIdentity();
    testConnectedMonotonizerFallbacksAreInertAtCompile();
    testSimplePitchShifterGraphCompilesWithAudioOutputBuffer();
    testInvalidPanParametersFailCompile();
    testInvalidDynamicRangeControllerParametersFailCompile();
    testInvalidReverbSectionParametersFailCompile();
    testReverbSectionGraphCompilesWithConnectedDelayScale();
    testReverbSectionConnectedControlsOwnInvalidDirectFallbacks();
    testStereoMix2GraphCompilesWithStereoOutputBuffers();
    testStereoMix4GraphCompilesWithStereoOutputBuffers();
    testOverlappingMixerGraphCompilesWithStereoOutputBuffersAndActiveInputs();
    testOverlappingMixerGraphCompilesWithNoActiveInputsAsSilence();
    testAnalogSequencerGraphCompilesWithStereoOutputBuffers();
    testConnectedAnalogSequencerControlsPrecedeVoiceClonesAndPreserveScopes();
    testOscilloscopeDisplayGraphCompilesAsPassthrough();
    testOscilloscopeDisplayBindsDirectSoundTrigger();
    testOscilloscopeDisplayRejectsOutOfRangeSweepFrequency();
    testOscilloscopeDisplayPreparesSubAudioInitialSweepFrequency();
    testOscilloscopeDisplayRejectsOutOfRangeLevel();
    testOscilloscopeDisplayBindsRightTriggerChannel();
    testOscilloscopeDisplayAuthorsFallingTriggerMode();
    testSpectrumAnalyzerDisplayGraphCompilesAsPassthrough();
    testDisplayGraphCompilesPreparedAnalysisProbesWhenEnabled();
    testExactNestedAnalysisTargetCompilesToOnePreparedProbe();
    testAnalysisTargetResolutionFailsClosed();
    testExactTypedEventAnalysisTargetRequiresDescriptorAuthority();
    testExactControlAnalysisTargetPublishesPreparedValues();
    testExactPartialSpectrumAnalysisTargetPublishesPreparedFrame();
    testExactComplexAnalysisTargetPublishesPreparedFrame();
    testInvalidStereoMix2ParametersFailCompile();
    testInvalidStereoMix4ParametersFailCompile();
    testInvalidOverlappingMixerParametersFailCompile();
    testInvalidAnalogSequencerParametersFailCompile();
    testInvalidForcedProcessorAssignmentParametersFailCompile();
    testInvalidPulseTrainParametersFailCompile();
    testInvalidPulseGeneratorParametersFailCompile();
    testInvalidIteratedWaveshaperParametersFailCompile();
    testInvalidShelvingFilterCutoffFailsCompile();
    testInvalidPresenceFilterParametersFailCompile();
    testInvalidTwoFormantElementParametersFailCompile();
    testInvalidTwoFormantVoiceElementParametersFailCompile();
    testInvalidHarmonicResonatorParametersFailCompile();
    testInvalidReResonatorParametersFailCompile();
    testInvalidGraphicEqParametersFailCompile();
    testInvalidVcfParametersFailCompile();
    testInvalidSingleSideBandRmParametersFailCompile();
    testInvalidPhaseShiftBy90ParametersFailCompile();
    testInvalidAnalysisFilterParametersFailCompile();
    testInvalidSumOfSinesSubsetParametersFailCompile();
    testInvalidGaOscillatorsParametersFailCompile();
    testInvalidFilterBankSubsetParametersFailCompile();
    testInvalidFormantBankSubsetParametersFailCompile();
    testInvalidVocoderSubsetParametersFailCompile();
    testInvalidBlankScriptFamilySubsetParametersFailCompile();
    testInvalidFeedbackLoopOutputSubsetParametersFailCompile();
    testInvalidScriptSubsetParametersFailCompile();
    testInvalidParameterTransformerSubsetParametersFailCompile();
    testInvalidFftSubsetParametersFailCompile();
    testInvalidSyntheticSpectrumSubsetParametersFailCompile();
    testInvalidSyntheticSpectrumFromSoundsSubsetParametersFailCompile();
    testInvalidLiveSpectralAnalysisSubsetParametersFailCompile();
    testInvalidSpectrumFileSourceSubsetParametersFailCompile();
    testInvalidSpectrumTrackSelectorSubsetParametersFailCompile();
    testInvalidSpectrumFrequencyScaleSubsetParametersFailCompile();
    testInvalidSpectralShapeSubsetParametersFailCompile();
    testInvalidSpectrumModifierSubsetParametersFailCompile();
    testInvalidBlankMidiInSubsetParametersFailCompile();
    testInvalidNoAudioTerminalSubsetParametersFailCompile();
    testInvalidStateSinkTerminalSubsetParametersFailCompile();
    testInvalidFrequencyScaleParametersFailCompile();
    testInvalidTimeFrequencyScaleParametersFailCompile();
    testInvalidMonotonizerParametersFailCompile();
    testInvalidSimplePitchShifterParametersFailCompile();
    testInvalidQuadOscillatorParametersFailCompile();
    testInvalidGenericSourceParametersFailCompile();
    testInvalidTimeControlParametersFailCompile();
    testInvalidFrequencyTrackerParametersFailCompile();
    testConnectedFrequencyTrackerControlsOwnInvalidFallbackRanges();
    testConnectedAveragingLowPassCutoffOwnsInvalidFallbackRange();
    testInvalidSampleParametersFailCompile();
    testInvalidDiskPlayerParametersFailCompile();
    testInvalidMultifileDiskPlayerParametersFailCompile();
    testInvalidSamplesFromDiskSingleStepParametersFailCompile();
    testInvalidMultisampleParametersFailCompile();
    testInvalidKeyMappedMultisampleParametersFailCompile();
    testMissingAudioOutputFailsCompile();
    testMultipleAudioOutputsFailCompile();
    testDuplicateConnectionsIntoSameInputFailCompile();
    testUnsupportedReachableNodeTypeFailsCompile();
    testInvalidRenderSettingsFailCompile();
    return 0;
}
