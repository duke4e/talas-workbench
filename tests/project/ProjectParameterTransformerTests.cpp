#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectParameterTransformer.h"
#include "talas/project/ProjectValidation.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.expression = std::move(source);
    return result;
}

talas::project::ProjectParameterExpression text(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression result;
    result.parameterId = std::move(id);
    result.text = std::move(source);
    return result;
}

talas::project::ProjectNode node(
    std::string id,
    std::string type,
    std::vector<talas::project::ProjectParameterExpression> parameters = {})
{
    return {std::move(id), std::move(type), std::move(parameters)};
}

talas::core::Connection connection(
    std::string from,
    std::string fromPort,
    std::string to,
    std::string toPort)
{
    return {
        std::move(from),
        std::move(fromPort),
        std::move(to),
        std::move(toPort),
    };
}

const talas::project::ProjectParameterExpression* findParameter(
    const talas::project::ProjectNode& node,
    std::string_view id)
{
    for (const auto& parameter : node.parameters)
        if (parameter.parameterId == id)
            return &parameter;
    return nullptr;
}

talas::project::ProjectDocument transformerDocument(
    std::string body,
    bool explicitFrequency = true)
{
    talas::project::ProjectDocument document;
    std::vector<talas::project::ProjectParameterExpression> oscillatorParameters;
    if (explicitFrequency)
        oscillatorParameters.push_back(expression("frequency", "100 hz"));
    oscillatorParameters.push_back(expression("amplitude", "0.5"));
    document.graph.nodes = {
        node("osc", "talas.oscillator", std::move(oscillatorParameters)),
        node("transform", "talas.parameterTransformer", {
            text("transformation", std::move(body)),
        }),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections = {
        connection("osc", "audioOut", "transform", "audioIn"),
        connection("transform", "audioOut", "out", "audioIn"),
    };
    return document;
}

void testParser()
{
    const auto parsed = talas::project::parseProjectParameterTransformation(
        "set frequency = 400 hz\r\nscale frequency by 2\n");
    require(
        parsed.ok() && parsed.rules.size() == 2
            && parsed.rules[0].kind
                == talas::project::ProjectParameterTransformationRuleKind::Set
            && parsed.rules[0].parameterId == "frequency"
            && parsed.rules[0].operand == "400 hz"
            && parsed.rules[0].sourceRange.offset == 0
            && parsed.rules[0].sourceRange.length == 22
            && parsed.rules[1].kind
                == talas::project::ProjectParameterTransformationRuleKind::Scale
            && parsed.rules[1].operand == "2",
        "set/scale parser should retain exact source ranges and CRLF semantics");

    // A Transformation is Smalltalk.
    const auto smalltalk = talas::project::parseProjectParameterTransformation(
        "snd frequency: 400 hz.");
    require(
        smalltalk.ok() && smalltalk.rules.empty() && smalltalk.program != nullptr,
        "a Smalltalk Transformation should parse into a program");
    const auto invalid = talas::project::parseProjectParameterTransformation(
        "snd frequency: (400 hz.");
    require(
        !invalid.ok()
            && invalid.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::InvalidSyntax,
        "a malformed Smalltalk Transformation should fail to parse");
    require(
        talas::project::parseProjectParameterTransformation("\n\r\n").ok(),
        "blank transformations should remain valid passthroughs");

    std::string invalidUtf8{"set frequency = "};
    invalidUtf8.push_back(static_cast<char>(0xC3));
    require(
        !talas::project::parseProjectParameterTransformation(invalidUtf8).ok(),
        "invalid UTF-8 must fail before rule parsing");

    std::string maximumRules;
    for (std::size_t index = 0;
         index < talas::project::maximumParameterTransformationRules;
         ++index)
    {
        maximumRules += "scale frequency by 1\n";
    }
    const auto exactLimit =
        talas::project::parseProjectParameterTransformation(maximumRules);
    require(
        exactLimit.ok()
            && exactLimit.rules.size()
                == talas::project::maximumParameterTransformationRules,
        "the exact rule-count boundary should remain accepted");
    maximumRules += "scale frequency by 1\n";
    require(
        !talas::project::parseProjectParameterTransformation(maximumRules).ok(),
        "one rule beyond the fixed boundary should fail closed");

    const auto replayA = talas::project::parseProjectParameterTransformation(
        "set frequency = 400 hz\nscale frequency by 2");
    const auto replayB = talas::project::parseProjectParameterTransformation(
        "set frequency = 400 hz\nscale frequency by 2");
    require(
        replayA.rules == replayB.rules,
        "closed parsing should replay deterministically");
}

void testSetScaleAndDefaults()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto prepared = talas::project::prepareProjectParameterTransformers(
        transformerDocument(
            "set frequency = 400 hz\n"
            "scale frequency by 2"),
        registry);
    require(prepared.ok() && prepared.transformedParameters == 2,
            "ordered set/scale rules should prepare one failure-atomic graph");
    const auto* frequency = findParameter(prepared.graph->nodes[0], "frequency");
    const auto* transformation =
        findParameter(prepared.graph->nodes[1], "transformation");
    require(
        frequency != nullptr && frequency->expression.has_value()
            && *frequency->expression == "(400 hz) * (2)"
            && transformation != nullptr
            && transformation->text == std::optional<std::string>{""},
        "compile-copy rules should compose and clear only prepared transformation text");

    const auto materialized = talas::project::prepareProjectParameterTransformers(
        transformerDocument("scale frequency by 2", false), registry);
    const auto* defaultFrequency = materialized.ok()
        ? findParameter(materialized.graph->nodes[0], "frequency")
        : nullptr;
    require(
        defaultFrequency != nullptr
            && defaultFrequency->expression.has_value()
            && *defaultFrequency->expression == "(440 hz) * (2)",
        "scale should materialize the descriptor default before rewriting");
}

// A Smalltalk Transformation runs on each
// Sound of the Input before it plays, reading and setting its parameters by
// name, with time and totalDuration in microseconds.
void testSmalltalkTransformations()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto field = [](const talas::project::ProjectParameterTransformationResult& prepared,
                           std::string_view id) {
        const auto* parameter = prepared.ok() ? findParameter(prepared.graph->nodes[0], id) : nullptr;
        return parameter != nullptr && parameter->expression.has_value() ? *parameter->expression
                                                                         : std::string{};
    };
    talas::project::ProjectParameterTransformationClock clock;
    const auto doubled = talas::project::prepareProjectParameterTransformers(
        transformerDocument("snd frequency isNil ifFalse: [snd frequency: snd frequency * 2]."),
        registry, clock);
    require(doubled.ok() && field(doubled, "frequency") == "200 hz" && field(doubled, "amplitude") == "0.5"
            && doubled.transformedParameters == 1,
        "a Transformation should double the Frequency and leave the rest alone");
    const auto* transformation = findParameter(doubled.graph->nodes[1], "transformation");
    require(transformation != nullptr && transformation->text == std::optional<std::string>{""},
        "a prepared Transformation should leave a passthrough behind");

    const auto set = talas::project::prepareProjectParameterTransformers(
        transformerDocument("snd amplitude: 0.25. snd missing: 3."), registry, clock);
    require(set.ok() && field(set, "amplitude") == "0.25" && set.transformedParameters == 1,
        "a Transformation should set the parameters a Sound has and pass over the rest");

    clock.startSeconds = [](const talas::project::ProjectGraph&, std::string_view nodeId, std::string_view) {
        return nodeId == "osc" ? std::optional<double>{0.5} : std::nullopt;
    };
    clock.durationSeconds = [](const talas::project::ProjectGraph&, std::string_view) {
        return std::optional<double>{2.0};
    };
    const auto timed = talas::project::prepareProjectParameterTransformers(
        transformerDocument("snd frequency: snd frequency * (1 + (time / totalDuration))."), registry, clock);
    require(timed.ok() && field(timed, "frequency") == "125 hz",
        "time and totalDuration should count microseconds");
    // Time is the Sound's start and totalDuration the Input's
    // duration, in microseconds: here 0.5 s and 2 s.
    const auto counted = talas::project::prepareProjectParameterTransformers(
        transformerDocument("snd amplitude: time / 1000000. snd frequency: (totalDuration / 1000) hz."), registry, clock);
    require(counted.ok() && field(counted, "amplitude") == "0.5" && field(counted, "frequency") == "2000 hz",
        "time and totalDuration should be 500000 and 2000000 microseconds");

    for (const auto* failing : {"snd frequency: 'loud'.", "snd frequency: nil + 1.", "snd frequency: undefined."})
    {
        const auto failed = talas::project::prepareProjectParameterTransformers(
            transformerDocument(failing), registry, clock);
        require(!failed.ok() && !failed.diagnostics.empty()
                && failed.diagnostics.front().code
                    == talas::project::ProjectParameterTransformationDiagnosticCode::InvalidExpression,
            "a Transformation that fails should name its failure and change nothing");
    }
}

// A transformation goes down each level of its Input's
// structure unless snd doNotTransformSubSounds stops it below a Sound (snd
// transformSubSounds goes on again), and snd doNotTransformThisSound passes
// over the current Sound while its inputs are still transformed.
void testSkippingLevels()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectParameterTransformationClock clock;
    const auto transformed = [&](std::string source) {
        auto nested = transformerDocument(source);
        nested.graph.nodes.insert(nested.graph.nodes.begin() + 1, node("gain", "talas.vca", {expression("gain", "0.5")}));
        nested.graph.connections = {
            connection("osc", "audioOut", "gain", "audioIn"),
            connection("gain", "audioOut", "transform", "audioIn"),
            connection("transform", "audioOut", "out", "audioIn"),
        };
        auto prepared = talas::project::prepareProjectParameterTransformers(nested, registry, clock);
        require(prepared.ok(), ("the Transformation should prepare: " + source).c_str());
        const auto value = [&](std::size_t index, std::string_view id) {
            const auto* parameter = findParameter(prepared.graph->nodes[index], id);
            return parameter != nullptr && parameter->expression.has_value() ? *parameter->expression : std::string{};
        };
        return std::pair{value(1, "gain"), value(0, "frequency")};
    };
    require(transformed("snd gain isNil ifFalse: [snd gain: 0.25. snd doNotTransformSubSounds]. "
                        "snd frequency isNil ifFalse: [snd frequency: 300 hz].")
            == std::pair<std::string, std::string>{"0.25", "100 hz"},
        "doNotTransformSubSounds should stop the transformation below the Sound");
    require(transformed("snd doNotTransformSubSounds. snd transformSubSounds. "
                        "snd frequency isNil ifFalse: [snd frequency: 300 hz].")
            == std::pair<std::string, std::string>{"0.5", "300 hz"},
        "transformSubSounds should go on to the inputs");
    require(transformed("snd gain isNil ifFalse: [snd doNotTransformThisSound]. snd gain: 0.25. "
                        "snd frequency isNil ifFalse: [snd frequency: 300 hz].")
            == std::pair<std::string, std::string>{"0.5", "300 hz"},
        "doNotTransformThisSound should skip the Sound and transform its inputs");
}

void testClosureAndFailureAtomicity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto nested = transformerDocument("scale frequency by 2");
    nested.graph.nodes.insert(
        nested.graph.nodes.begin() + 1,
        node("gain", "talas.vca", {expression("gain", "0.5")}));
    nested.graph.connections = {
        connection("osc", "audioOut", "gain", "audioIn"),
        connection("gain", "audioOut", "transform", "audioIn"),
        connection("transform", "audioOut", "out", "audioIn"),
    };
    const auto prepared =
        talas::project::prepareProjectParameterTransformers(nested, registry);
    require(
        prepared.ok()
            && *findParameter(prepared.graph->nodes[0], "frequency")->expression
                == "(100 hz) * (2)",
        "transformer should walk the complete exclusive upstream closure");

    auto shared = transformerDocument("scale frequency by 2");
    shared.graph.nodes.push_back(node("otherOut", "talas.audioOutput"));
    shared.graph.connections.push_back(
        connection("osc", "audioOut", "otherOut", "audioIn"));
    const auto original = shared.graph;
    const auto rejected =
        talas::project::prepareProjectParameterTransformers(shared, registry);
    require(
        !rejected.ok() && !rejected.graph.has_value()
            && rejected.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::SharedUpstreamBranch
            && shared.graph == original,
        "shared upstream input must reject without mutating authored graph authority");

    const auto unmatched = talas::project::prepareProjectParameterTransformers(
        transformerDocument("set missingParameter = 1"), registry);
    require(
        !unmatched.ok()
            && unmatched.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::UnmatchedParameter,
        "a rule that matches no upstream Expression parameter must fail visibly");

    talas::project::ProjectDocument structural;
    structural.graph.nodes = {
        node("voice", "talas.midiVoice", {expression("polyphony", "8")}),
        node("transform", "talas.parameterTransformer", {
            text("transformation", "set polyphony = 16"),
        }),
        node("out", "talas.audioOutput"),
    };
    structural.graph.connections = {
        connection("voice", "leftOut", "transform", "audioIn"),
        connection("transform", "audioOut", "out", "audioIn"),
    };
    const auto structuralRejected =
        talas::project::prepareProjectParameterTransformers(
            structural,
            registry);
    require(
        !structuralRejected.ok()
            && structuralRejected.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::UnsupportedParameter,
        "MIDI Voice polyphony must fail closed after voice-scope materialization");

    auto chained = transformerDocument("set frequency = 200 hz");
    chained.graph.nodes.insert(
        chained.graph.nodes.begin() + 2,
        node("downstreamTransform", "talas.parameterTransformer", {
            text("transformation", "scale frequency by 3"),
        }));
    chained.graph.connections = {
        connection("osc", "audioOut", "transform", "audioIn"),
        connection(
            "transform",
            "audioOut",
            "downstreamTransform",
            "audioIn"),
        connection(
            "downstreamTransform",
            "audioOut",
            "out",
            "audioIn"),
    };
    const auto chainedPrepared =
        talas::project::prepareProjectParameterTransformers(
            chained,
            registry);
    require(
        chainedPrepared.ok()
            && *findParameter(
                    chainedPrepared.graph->nodes[0],
                    "frequency")->expression
                == "(200 hz) * (3)",
        "chained transformers should apply in upstream-to-downstream source order");

    auto cyclic = transformerDocument("scale frequency by 2");
    cyclic.graph.nodes.insert(
        cyclic.graph.nodes.begin() + 1,
        node("gain", "talas.vca", {expression("gain", "0.5")}));
    cyclic.graph.connections = {
        connection("osc", "audioOut", "gain", "audioIn"),
        connection("gain", "audioOut", "osc", "reset"),
        connection("gain", "audioOut", "transform", "audioIn"),
        connection("transform", "audioOut", "out", "audioIn"),
    };
    const auto cyclicRejected =
        talas::project::prepareProjectParameterTransformers(
            cyclic,
            registry);
    require(
        !cyclicRejected.ok()
            && cyclicRejected.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::InvalidGraph,
        "cyclic upstream closure must fail closed before rewrite");
}

void testEveryFixedCapacityBoundary()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();

    const std::string exactSource(
        talas::project::maximumParameterTransformationSourceBytes,
        '\n');
    require(
        talas::project::parseProjectParameterTransformation(exactSource).ok(),
        "the exact transformation-source byte limit should remain accepted");
    require(
        !talas::project::parseProjectParameterTransformation(
             exactSource + "\n").ok(),
        "one byte beyond the transformation-source limit should fail closed");

    talas::project::ProjectDocument transformerCount;
    for (std::size_t index = 0;
         index < talas::project::maximumParameterTransformerNodes;
         ++index)
    {
        transformerCount.graph.nodes.push_back(node(
            "transform" + std::to_string(index),
            "talas.parameterTransformer",
            {text("transformation", "scale frequency by 1")}));
    }
    const auto exactTransformers =
        talas::project::prepareProjectParameterTransformers(
            transformerCount,
            registry);
    require(
        !exactTransformers.ok()
            && !exactTransformers.diagnostics.empty()
            && exactTransformers.diagnostics.front().code
                != talas::project::ProjectParameterTransformationDiagnosticCode::TooManyTransformers,
        "64 transformer nodes should pass the transformer-count boundary");
    transformerCount.graph.nodes.push_back(node(
        "transform-over-limit",
        "talas.parameterTransformer",
        {text("transformation", "scale frequency by 1")}));
    const auto tooManyTransformers =
        talas::project::prepareProjectParameterTransformers(
            transformerCount,
            registry);
    require(
        !tooManyTransformers.ok()
            && tooManyTransformers.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::TooManyTransformers,
        "the 65th transformer node should fail at the exact count guard");

    auto writeDocument = [](std::size_t oscillatorCount) {
        talas::project::ProjectDocument document;
        document.graph.nodes.reserve(oscillatorCount + 1);
        document.graph.connections.reserve(oscillatorCount);
        for (std::size_t index = 0; index < oscillatorCount; ++index)
        {
            document.graph.nodes.push_back(node(
                "osc" + std::to_string(index),
                "talas.oscillator",
                {expression("frequency", "1 hz")}));
            if (index != 0)
            {
                document.graph.connections.push_back(connection(
                    "osc" + std::to_string(index - 1),
                    "audioOut",
                    "osc" + std::to_string(index),
                    "audioIn"));
            }
        }
        document.graph.nodes.push_back(node(
            "transform",
            "talas.parameterTransformer",
            {text("transformation", "scale frequency by 1")}));
        document.graph.connections.push_back(connection(
            "osc" + std::to_string(oscillatorCount - 1),
            "audioOut",
            "transform",
            "audioIn"));
        return document;
    };
    const auto exactWrites = talas::project::prepareProjectParameterTransformers(
        writeDocument(talas::project::maximumParameterTransformationWrites),
        registry);
    require(
        exactWrites.ok()
            && exactWrites.transformedParameters
                == talas::project::maximumParameterTransformationWrites,
        "the exact 4096 transformed-write boundary should remain accepted");
    const auto tooManyWrites =
        talas::project::prepareProjectParameterTransformers(
            writeDocument(
                talas::project::maximumParameterTransformationWrites + 1),
            registry);
    require(
        !tooManyWrites.ok()
            && tooManyWrites.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
        "the 4097th transformed write should fail closed");

    talas::project::ProjectDocument traversal;
    constexpr std::size_t traversalNodeCount = 32768;
    traversal.graph.nodes.reserve(traversalNodeCount);
    traversal.graph.connections.reserve(traversalNodeCount + 1);
    traversal.graph.nodes.push_back(node(
        "source",
        "talas.oscillator",
        {expression("frequency", "1 hz")}));
    for (std::size_t index = 1; index + 1 < traversalNodeCount; ++index)
    {
        traversal.graph.nodes.push_back(node(
            "filler" + std::to_string(index),
            "talas.audioOutput"));
        traversal.graph.connections.push_back(connection(
            index == 1 ? "source" : "filler" + std::to_string(index - 1),
            "audioOut",
            "filler" + std::to_string(index),
            "audioIn"));
    }
    traversal.graph.nodes.push_back(node(
        "transform",
        "talas.parameterTransformer",
        {text("transformation", "scale frequency by 1")}));
    traversal.graph.connections.push_back(connection(
        "filler" + std::to_string(traversalNodeCount - 2),
        "audioOut",
        "transform",
        "audioIn"));
    traversal.graph.connections.push_back(connection(
        "filler" + std::to_string(traversalNodeCount - 2),
        "audioOut",
        "transform",
        "ignored"));
    require(
        traversal.graph.nodes.size()
                + traversal.graph.connections.size()
            == talas::project::maximumParameterTransformationTraversalRecords,
        "traversal fixture should land on the exact aggregate boundary");
    require(
        talas::project::prepareProjectParameterTransformers(
            traversal,
            registry).ok(),
        "the exact 65536-record graph boundary should remain accepted");
    traversal.graph.connections.push_back(connection(
        "source",
        "audioOut",
        "transform",
        "ignored2"));
    const auto tooLarge = talas::project::prepareProjectParameterTransformers(
        traversal,
        registry);
    require(
        !tooLarge.ok()
            && tooLarge.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::GraphTooLarge,
        "one graph record beyond 65536 should fail before traversal allocation");

    constexpr std::size_t cumulativeTraversalNodes = 16384;
    auto cumulativeTraversal =
        writeDocument(cumulativeTraversalNodes);
    cumulativeTraversal.graph.nodes.back().instanceId = "transform1";
    cumulativeTraversal.graph.connections.back().toNode = "transform1";
    cumulativeTraversal.graph.nodes.push_back(node(
        "transform2",
        "talas.parameterTransformer",
        {text("transformation", "scale frequency by 1")}));
    cumulativeTraversal.graph.connections.push_back(connection(
        "transform1",
        "audioOut",
        "transform2",
        "audioIn"));
    const auto exactTraversal =
        talas::project::prepareProjectParameterTransformers(
            cumulativeTraversal,
            registry);
    require(
        !exactTraversal.ok()
            && !exactTraversal.diagnostics.empty()
            && exactTraversal.diagnostics.front().message.find(
                   "4096-write") != std::string::npos,
        "the exact cumulative 65536-record traversal boundary should reach the later write guard");
    cumulativeTraversal.graph.connections.push_back(connection(
        "osc0",
        "audioOut",
        "transform1",
        "ignored"));
    const auto traversalOverflow =
        talas::project::prepareProjectParameterTransformers(
            cumulativeTraversal,
            registry);
    require(
        !traversalOverflow.ok()
            && !traversalOverflow.diagnostics.empty()
            && traversalOverflow.diagnostics.front().message.find(
                   "traversal exceeded") != std::string::npos,
        "the 65537th cumulative traversal record should fail at the traversal counter");

    auto expressionBoundary = transformerDocument("scale frequency by 1");
    auto frequency = std::ranges::find(
        expressionBoundary.graph.nodes[0].parameters,
        std::string_view{"frequency"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        frequency != expressionBoundary.graph.nodes[0].parameters.end(),
        "expression boundary fixture needs frequency");
    frequency->expression = "1" + std::string(
        talas::expr::ExpressionLimits::maximumSourceBytes - 9,
        ' ');
    require(
        talas::project::prepareProjectParameterTransformers(
            expressionBoundary,
            registry).ok(),
        "a rewritten expression at the exact source boundary should remain accepted");
    frequency->expression->push_back(' ');
    const auto tooLong = talas::project::prepareProjectParameterTransformers(
        expressionBoundary,
        registry);
    require(
        !tooLong.ok()
            && tooLong.diagnostics.front().code
                == talas::project::ProjectParameterTransformationDiagnosticCode::RewrittenExpressionTooLong,
        "one rewritten-expression byte beyond the fixed limit should fail before allocation");
}

void testAuthoringAndPersistenceUseTheClosedParser()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = transformerDocument("");
    talas::project::ProjectEditor editor{document, registry};
    const auto invalidEdit = editor.setParameter(
        "transform",
        text("transformation", "snd frequency: (400 hz"));
    require(
        !invalidEdit.ok()
            && invalidEdit.error
                == talas::project::EditError::InvalidParameterValue
            && editor.document().graph == document.graph,
        "ProjectEditor must reject malformed transformation syntax without mutation");
    require(
        editor.setParameter(
            "transform",
            text("transformation", "scale frequency by 2")).ok(),
        "ProjectEditor should accept the closed transformation language");
    require(
        !editor.setParameter(
             "transform",
             text("transformation", "set frequency = 1 +")).ok(),
        "ProjectEditor should reject an invalid set expression before commit");
    require(
        !editor.setParameter(
             "transform",
             text("transformation", "set missingParameter = 1")).ok(),
        "ProjectEditor should reject structurally unmatched rules before commit");
    require(
        !editor.setParameter(
             "transform",
             text("transformation", "set interpolation = 1")).ok(),
        "ProjectEditor should reject non-Expression parameter targets before commit");
    require(
        editor.addNode(node("outside", "talas.vca"), {0, 0}).ok(),
        "shared-branch editor fixture should add its outside consumer");
    require(
        !editor.connect(connection(
             "osc",
             "audioOut",
             "outside",
             "audioIn")).ok(),
        "ProjectEditor should reject a topology edit that shares an active transformer closure");

    document.graph.nodes[1].parameters[0] =
        text("transformation", "snd frequency: (400 hz");
    const auto persistence =
        talas::project::validateProjectDocumentForPersistence(document);
    require(
        !persistence.ok()
            && persistence.diagnostics.front().code
                == talas::project::ProjectPersistenceValidationCode::InvalidParameterTransformation,
        "persistence must reject transformation text the compile parser rejects");

    document.graph.nodes[1].parameters[0] =
        text("transformation", "set frequency = 1 +");
    require(
        !talas::project::validateProjectDocumentForPersistence(document).ok(),
        "persistence must reject an invalid set expression before serialization");

    document = transformerDocument("set missingParameter = 1");
    require(
        !talas::project::validateProjectDocumentForPersistence(document).ok(),
        "persistence must reject structurally unmatched rules before serialization");

    document = transformerDocument("set interpolation = 1");
    require(
        !talas::project::validateProjectDocumentForPersistence(document).ok(),
        "persistence must reject non-Expression parameter targets before serialization");

    document = transformerDocument("scale frequency by 2");
    document.graph.nodes.push_back(node("outside", "talas.vca"));
    document.graph.connections.push_back(connection(
        "osc",
        "audioOut",
        "outside",
        "audioIn"));
    require(
        !talas::project::validateProjectDocumentForPersistence(document).ok(),
        "persistence must reject shared transformer closures before serialization");

    document = transformerDocument(
        "set frequency = 400 hz\nscale frequency by 2");
    const auto persistenceOk =
        talas::project::validateProjectDocumentForPersistence(document);
    require(
        persistenceOk.ok(),
        "valid nonblank transformation text should pass persistence preflight");
    const auto canonical =
        talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    const auto* reopenedTransformation = reopened.ok()
        ? findParameter(
              reopened.document.graph.nodes[1],
              "transformation")
        : nullptr;
    require(
        reopened.ok()
            && reopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && reopenedTransformation != nullptr
            && reopenedTransformation->text
                == std::optional<std::string>{
                    "set frequency = 400 hz\nscale frequency by 2"},
        "format-32 canonical save/reopen should preserve exact transformation text");
}

void testAuthoredPreflightExpandsReusableSoundsWithoutRecursion()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "transform",
        "audioOut",
    });
    definition.graph.nodes = {
        node("osc", "talas.oscillator", {
            expression("frequency", "100 hz"),
        }),
        node("transform", "talas.parameterTransformer", {
            text("transformation", "scale frequency by 2"),
        }),
    };
    definition.graph.connections.push_back(connection(
        "osc",
        "audioOut",
        "transform",
        "audioIn"));
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode voice;
    voice.instanceId = "voice1";
    voice.soundDefinitionId = "voice";
    document.graph.nodes = {
        std::move(voice),
        node("out", "talas.audioOutput"),
    };
    document.graph.connections.push_back(connection(
        "voice1",
        "audioOut",
        "out",
        "audioIn"));

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto prepared =
        talas::project::prepareAuthoredProjectParameterTransformers(
            document,
            registry);
    require(
        prepared.ok() && prepared.transformedParameters == 1,
        "authored preflight should expand and transform a reusable Sound without recursive validation");
    const auto transformedOscillator = std::ranges::find_if(
        prepared.graph->nodes,
        [](const auto& candidate) {
            return candidate.typeId == "talas.oscillator";
        });
    require(
        transformedOscillator != prepared.graph->nodes.end()
            && findParameter(*transformedOscillator, "frequency") != nullptr
            && findParameter(*transformedOscillator, "frequency")->expression
                == std::optional<std::string>{"(100 hz) * (2)"},
        "reusable-Sound preflight should rewrite the expanded oscillator copy");
    require(
        talas::project::validateProjectDocumentForEditing(document, registry).ok()
            && talas::project::validateProjectDocumentForPersistence(document).ok(),
        "valid reusable-Sound transformation should pass editor and persistence preflight");

    auto unused = document;
    unused.graph = {};
    talas::project::ProjectEditor unusedEditor{unused, registry};
    const auto before = talas::project::serializeProjectDocument(
        unusedEditor.document());
    require(
        !unusedEditor.setParameterInSoundDefinition(
             "voice",
             "transform",
             text("transformation", "set missingParameter = 1")).ok()
            && talas::project::serializeProjectDocument(
                   unusedEditor.document()) == before,
        "editor preflight must reject an invalid transformer in an uninstantiated reusable Sound");
    unused.soundDefinitions.front().graph.nodes[1].parameters[0] =
        text("transformation", "set missingParameter = 1");
    require(
        !talas::project::validateProjectDocumentForPersistence(unused).ok(),
        "persistence must reject an invalid transformer in an uninstantiated reusable Sound");

    talas::project::ProjectDocument effect;
    talas::project::ProjectSoundDefinition effectDefinition;
    effectDefinition.definitionId = "effect";
    effectDefinition.displayName = "Effect";
    effectDefinition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "transform",
        "audioIn",
    });
    effectDefinition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "transform",
        "audioOut",
    });
    effectDefinition.graph.nodes.push_back(node(
        "transform",
        "talas.parameterTransformer",
        {text(
            "transformation",
            "set callerFrequency = 220 hz")}));
    effect.soundDefinitions.push_back(std::move(effectDefinition));
    require(
        talas::project::validateProjectDocumentForEditing(effect, registry).ok()
            && talas::project::validateProjectDocumentForPersistence(effect).ok(),
        "an uninstantiated effect Sound should defer transformer matching across its open audio-input boundary");

    auto malformedEffect = effect;
    malformedEffect.soundDefinitions.front().graph.nodes.front()
        .parameters.front() = text(
            "transformation",
            "snd frequency: (400 hz");
    require(
        !talas::project::validateProjectDocumentForEditing(
             malformedEffect,
             registry).ok()
            && !talas::project::validateProjectDocumentForPersistence(
                    malformedEffect).ok(),
        "an open effect boundary may defer structural matching but must still reject malformed transformation source");

    talas::project::ProjectDocument nestedEffect;
    nestedEffect.soundDefinitions.push_back(
        effect.soundDefinitions.front());
    auto outer = talas::project::ProjectSoundDefinition{};
    outer.definitionId = "outer";
    outer.displayName = "Outer";
    outer.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "nested",
        "audioIn",
    });
    outer.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "nested",
        "audioOut",
    });
    talas::project::ProjectNode nested;
    nested.instanceId = "nested";
    nested.soundDefinitionId = "effect";
    outer.graph.nodes = {
        std::move(nested),
        node("osc", "talas.oscillator", {
            expression("frequency", "100 hz"),
        }),
        node("closedTransform", "talas.parameterTransformer", {
            text("transformation", "scale frequency by 2"),
        }),
    };
    outer.graph.connections.push_back(connection(
        "osc",
        "audioOut",
        "closedTransform",
        "audioIn"));
    nestedEffect.soundDefinitions.push_back(std::move(outer));
    require(
        talas::project::validateProjectDocumentForEditing(
            nestedEffect,
            registry).ok()
            && talas::project::validateProjectDocumentForPersistence(
                nestedEffect).ok(),
        "a closed dormant transformer should remain valid beside a nested effect Sound's deferred open boundary");

    auto closedNestedEffect = nestedEffect;
    auto& innerDefinition = closedNestedEffect.soundDefinitions.front();
    innerDefinition.graph.nodes.front().parameters.front() = text(
        "transformation",
        "set missingParameter = 1");
    auto& outerDefinition = closedNestedEffect.soundDefinitions.back();
    outerDefinition.inputs.clear();
    outerDefinition.graph.nodes = {
        std::move(outerDefinition.graph.nodes.front()),
        node("nestedSource", "talas.oscillator", {
            expression("frequency", "120 hz"),
        }),
    };
    outerDefinition.graph.connections = {
        connection(
            "nestedSource",
            "audioOut",
            "nested",
            "audioIn"),
    };
    require(
        !talas::project::validateProjectDocumentForEditing(
             closedNestedEffect,
             registry).ok()
            && !talas::project::validateProjectDocumentForPersistence(
                    closedNestedEffect).ok(),
        "a nested effect transformer must be fully validated when its outer use closes the audio-input boundary");

    auto unrelatedLarge = effect;
    talas::project::ProjectSoundDefinition unrelated;
    unrelated.definitionId = "unrelated";
    unrelated.displayName = "Unrelated";
    unrelated.graph.nodes.reserve(8193);
    for (std::size_t index = 0; index < 8193; ++index)
    {
        unrelated.graph.nodes.push_back(node(
            "unrelated" + std::to_string(index),
            "talas.oscillator"));
    }
    unrelatedLarge.soundDefinitions.push_back(std::move(unrelated));
    require(
        talas::project::validateProjectDocumentForEditing(
            unrelatedLarge,
            registry).ok()
            && talas::project::validateProjectDocumentForPersistence(
                unrelatedLarge).ok(),
        "an unrelated dormant Sound beyond one synthetic expansion budget must not affect transformer validity elsewhere");
}

void testPersistenceUsesThePreservedComponentInterface()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.0.0";
    component.displayName = "Preserved oscillator";
    component.bundleId = "org.example.preserved-oscillator";
    component.recoveryHint = "Install the matching component";
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        talas::core::defaultPortSignalContract(talas::core::PortType::Audio),
    });
    component.parameters.push_back({
        "frequency",
        talas::core::ParameterKind::Expression,
        talas::expr::ValueDomain::Hertz,
        {},
    });

    auto document = transformerDocument("scale frequency by 2");
    document.graph.nodes.front().typeId =
        "org.example.preserved-oscillator";
    document.graph.nodes.front().preservedComponent = std::move(component);

    const auto editing = talas::project::validateProjectDocumentForEditing(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(
        editing.ok(),
        "editing preflight should plan through a preserved component's frozen interface");
    require(
        talas::project::validateProjectDocumentForPersistence(document).ok(),
        "persistence must retain a structurally valid transformation over a preserved component");

    auto externalRegistry = talas::core::makeBuiltinNodeRegistry();
    talas::core::NodeTypeDescriptor external;
    external.typeId = "org.example.external-oscillator";
    external.displayName = "External oscillator";
    external.category = "External";
    external.provenance.vendor = "Example vendor";
    external.provenance.origin = talas::core::NodeOrigin::External;
    external.outputs.emplace_back(
        "audioOut",
        talas::core::PortType::Audio,
        false);
    talas::core::ParameterDescriptor frequency;
    frequency.id = "frequency";
    frequency.defaultExpression = "100 hz";
    frequency.domain = talas::expr::ValueDomain::Hertz;
    external.parameters.push_back(std::move(frequency));
    externalRegistry.addNodeType(std::move(external));

    talas::project::ProjectEditor editor{{}, externalRegistry};
    require(
        editor.addNode(node(
            "external",
            "org.example.external-oscillator",
            {expression("frequency", "100 hz")})).ok()
            && editor.document().graph.nodes.front().preservedComponent
                .has_value(),
        "authoring an external component should freeze its exact interface into the project");
    require(
        editor.addNode(node(
            "transform",
            "talas.parameterTransformer",
            {text("transformation", "")})).ok()
            && editor.addNode(node("out", "talas.audioOutput")).ok()
            && editor.connect(connection(
                "external",
                "audioOut",
                "transform",
                "audioIn")).ok()
            && editor.connect(connection(
                "transform",
                "audioOut",
                "out",
                "audioIn")).ok()
            && editor.setParameter(
                "transform",
                text("transformation", "scale frequency by 2")).ok(),
        "external-component transformer fixture should be accepted by the active registry");
    require(
        talas::project::validateProjectDocumentForPersistence(
            editor.document()).ok(),
        "a project authored with an installed external component must remain persistable through its frozen interface");

    auto legacyExternal = editor.document();
    legacyExternal.graph.nodes.front().preservedComponent.reset();
    talas::project::ProjectEditor loadedLegacy{
        legacyExternal,
        externalRegistry};
    require(
        loadedLegacy.document().graph.nodes.front().preservedComponent
                .has_value()
            && talas::project::validateProjectDocumentForPersistence(
                loadedLegacy.document()).ok(),
        "constructing an editor should upgrade a registry-resolved legacy external node with a frozen interface");

    talas::project::ProjectEditor replacementEditor{
        talas::project::ProjectDocument{},
        externalRegistry};
    require(
        replacementEditor.replaceDocument(legacyExternal).ok()
            && replacementEditor.document().graph.nodes.front()
                .preservedComponent.has_value(),
        "whole-document replacement should freeze registry-resolved external interfaces before commit");
    talas::project::ProjectEditor wholeEditEditor{
        talas::project::ProjectDocument{},
        externalRegistry};
    require(
        wholeEditEditor.applyDocumentEdit(legacyExternal).ok()
            && wholeEditEditor.document().graph.nodes.front()
                .preservedComponent.has_value(),
        "whole-document edits should freeze registry-resolved external interfaces before commit");

    auto oversizedRegistry = talas::core::makeBuiltinNodeRegistry();
    auto oversizedExternal = talas::core::NodeTypeDescriptor{};
    oversizedExternal.typeId = "org.example.oversized-external";
    oversizedExternal.displayName = "Oversized external";
    oversizedExternal.category = "External";
    oversizedExternal.provenance.vendor = "Example vendor";
    oversizedExternal.provenance.origin = talas::core::NodeOrigin::External;
    for (std::size_t index = 0;
         index <= talas::project::maximumProjectPreservedComponentParameters;
         ++index)
    {
        talas::core::ParameterDescriptor parameter;
        parameter.id = "parameter" + std::to_string(index);
        parameter.defaultExpression = "0";
        oversizedExternal.parameters.push_back(std::move(parameter));
    }
    oversizedRegistry.addNodeType(std::move(oversizedExternal));
    talas::project::ProjectDocument oversizedDocument;
    oversizedDocument.graph.nodes.push_back(node(
        "external",
        "org.example.oversized-external"));
    bool constructorRejected = false;
    try
    {
        talas::project::ProjectEditor oversizedEditor{
            std::move(oversizedDocument),
            oversizedRegistry};
        static_cast<void>(oversizedEditor);
    }
    catch (const std::invalid_argument&)
    {
        constructorRejected = true;
    }
    require(
        constructorRejected,
        "ProjectEditor construction must fail closed when an external interface cannot be frozen losslessly");
}
}

int main()
{
    testParser();
    testSetScaleAndDefaults();
    testSmalltalkTransformations();
    testSkippingLevels();
    testClosureAndFailureAtomicity();
    testEveryFixedCapacityBoundary();
    testAuthoringAndPersistenceUseTheClosedParser();
    testAuthoredPreflightExpandsReusableSoundsWithoutRecursion();
    testPersistenceUsesThePreservedComponentInterface();
}
