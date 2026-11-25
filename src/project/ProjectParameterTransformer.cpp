#include "talas/project/ProjectParameterTransformer.h"

#include "talas/core/FromChars.h"
#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/project/ProjectSoundExpansion.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/SmalltalkSubset.h"

#include "JsonValue.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>
#include <new>
#include <ranges>
#include <span>
#include <unordered_map>
#include <utility>

namespace talas::project
{
namespace
{
[[nodiscard]] bool asciiSpace(char value) noexcept
{
    return value == ' ' || value == '\t';
}

[[nodiscard]] std::string_view trimAscii(std::string_view value) noexcept
{
    while (!value.empty() && asciiSpace(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && asciiSpace(value.back()))
        value.remove_suffix(1);
    return value;
}

[[nodiscard]] bool validParameterId(std::string_view value) noexcept
{
    if (value.empty()
        || value.size() > maximumParameterTransformationIdentifierBytes)
    {
        return false;
    }
    const auto alpha = [](char ch) noexcept {
        return (ch >= 'A' && ch <= 'Z')
            || (ch >= 'a' && ch <= 'z') || ch == '_';
    };
    const auto digit = [](char ch) noexcept {
        return ch >= '0' && ch <= '9';
    };
    if (!alpha(value.front()))
        return false;
    return std::ranges::all_of(value.substr(1), [&](char ch) {
        return alpha(ch) || digit(ch) || ch == '-';
    });
}

template <typename Result>
void addDiagnostic(
    Result& result,
    ProjectParameterTransformationDiagnosticCode code,
    std::string message,
    talas::expr::SourceRange range = {},
    std::string transformerNodeId = {},
    std::size_t ruleIndex = 0,
    std::string targetNodeId = {},
    std::string parameterId = {})
{
    if (result.diagnostics.size()
        >= maximumParameterTransformationDiagnostics)
    {
        result.truncated = true;
        return;
    }
    result.diagnostics.push_back({
        code,
        std::move(transformerNodeId),
        std::move(targetNodeId),
        std::move(parameterId),
        ruleIndex,
        range,
        std::move(message),
    });
}

[[nodiscard]] const talas::core::ParameterDescriptor* findDescriptorParameter(
    const talas::core::NodeTypeDescriptor& descriptor,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find(
        descriptor.parameters,
        parameterId,
        &talas::core::ParameterDescriptor::id);
    return found == descriptor.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] ProjectParameterExpression* findProjectParameter(
    ProjectNode& node,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find(
        node.parameters,
        parameterId,
        &ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] const ProjectParameterExpression* findProjectParameter(
    const ProjectNode& node,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find(
        node.parameters,
        parameterId,
        &ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] std::string_view transformationText(
    const ProjectNode& node,
    const talas::core::NodeTypeDescriptor& descriptor) noexcept
{
    if (const auto* parameter = findProjectParameter(node, "transformation");
        parameter != nullptr && parameter->text.has_value())
    {
        return *parameter->text;
    }
    const auto* parameter =
        findDescriptorParameter(descriptor, "transformation");
    return parameter == nullptr ? std::string_view{}
                                : std::string_view{parameter->defaultText};
}

struct PreparedTransformer
{
    std::size_t nodeIndex = 0;
    std::vector<ProjectParameterTransformationRule> rules;
    std::shared_ptr<const smalltalk::Program> program;
    std::vector<std::uint8_t> closure;
    std::size_t closureSize = 0;
    bool deferredOpenBoundary = false;
};

[[nodiscard]] ProjectParameterTransformationResult
prepareProjectParameterTransformersImpl(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::span<const std::string> openBoundaryNodeIds,
    std::size_t& traversalRecords,
    const ProjectParameterTransformationClock* clock);

[[nodiscard]] talas::expr::Unit unitOf(talas::expr::ValueDomain domain) noexcept
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Hertz:
        return talas::expr::Unit::Hertz;
    case talas::expr::ValueDomain::Seconds:
        return talas::expr::Unit::Seconds;
    case talas::expr::ValueDomain::NoteNumber:
        return talas::expr::Unit::NoteNumber;
    case talas::expr::ValueDomain::Samples:
        return talas::expr::Unit::Samples;
    default:
        return talas::expr::Unit::None;
    }
}

// The Sound a Transformation runs on, as snd: a
// parameter reads as the number its field holds, in the field's units, or
// as the expression it holds where that follows Event Values or a time,
// and as nil where the Sound has none; setting a parameter it has rewrites
// that field, and setting one it lacks does nothing.
class TransformedSound final : public smalltalk::Host
{
public:
    TransformedSound(
        const ProjectDocument& document,
        const talas::core::NodeTypeDescriptor& descriptor,
        ProjectNode& node,
        double sampleRate,
        std::optional<double> startSeconds,
        std::optional<double> totalSeconds)
        : document_(document)
        , descriptor_(descriptor)
        , node_(node)
        , sampleRate_(sampleRate)
        , startSeconds_(startSeconds)
        , totalSeconds_(totalSeconds)
    {
    }

    // A project before format 121 keeps every number floating point.
    bool exactNumbers() override { return !document_.smalltalkNumbersAreFloats; }
    // The project's rounded and truncated.
    talas::expr::RoundingRule rounding() override { return document_.expressionDefinitions.rounding; }

    std::size_t writes = 0;
    // Skipping levels: whether the transformation
    // goes on to this Sound's inputs, and whether this Sound keeps its own.
    bool transformSubSounds = true;
    bool transformThisSound = true;

    std::optional<smalltalk::Value> variable(std::string_view name) override
    {
        // time and totalDuration count microseconds.
        if (name == "snd")
            return smalltalk::Value::object(0);
        if (name == "time")
            return startSeconds_ ? smalltalk::Value::of(*startSeconds_ * 1'000'000.0) : smalltalk::Value::nil();
        if (name == "totalDuration")
            return totalSeconds_ ? smalltalk::Value::of(*totalSeconds_ * 1'000'000.0) : smalltalk::Value::nil();
        // A variable, ?name, with its declared value.
        if (name.starts_with('?'))
            return smalltalk::declaredVariable(document_.variableLibrary, name);
        return std::nullopt;
    }

    std::optional<smalltalk::Value> send(
        const smalltalk::Value& receiver,
        std::string_view selector,
        std::span<const smalltalk::Value> arguments,
        std::string& error) override
    {
        if (receiver.index == 0 && arguments.empty())
        {
            if (selector == "doNotTransformSubSounds" || selector == "transformSubSounds")
            {
                transformSubSounds = selector == "transformSubSounds";
                return receiver;
            }
            if (selector == "doNotTransformThisSound")
            {
                transformThisSound = false;
                return receiver;
            }
            // Whether the Sound expands first is decided before it runs.
            if (selector == "doNotExpandFirst" || selector == "expandFirst")
                return receiver;
        }
        const auto setting = selector.ends_with(':');
        if (receiver.index != 0 || (setting ? arguments.size() != 1 : !arguments.empty())
            || (setting && selector.find(':') != selector.size() - 1))
        {
            error = "snd does not understand " + std::string{selector};
            return std::nullopt;
        }
        const auto id = setting ? selector.substr(0, selector.size() - 1) : selector;
        const auto* parameter = findDescriptorParameter(descriptor_, id);
        if (parameter == nullptr)
            return setting ? receiver : smalltalk::Value::nil();
        if (!setting)
            return read(*parameter);
        if (!write(*parameter, arguments.front(), error))
            return std::nullopt;
        return receiver;
    }

private:
    smalltalk::Value read(const talas::core::ParameterDescriptor& parameter) const
    {
        const auto* value = findProjectParameter(node_, parameter.id);
        switch (parameter.kind)
        {
        case talas::core::ParameterKind::Expression:
        {
            const auto source = value != nullptr && value->expression.has_value()
                ? *value->expression
                : parameter.defaultExpression;
            const auto parsed = talas::expr::parse(source, document_.expressionLanguage, document_.expressionDefinitions);
            if (parsed.ok() && !parsed.expression->isStateful() && parsed.expression->eventReferences().empty()
                && parsed.expression->variableReferences().empty() && parsed.expression->identifiers().empty())
            {
                talas::expr::EvaluationContext context;
                context.sampleRate = sampleRate_;
                const auto evaluated = talas::expr::evaluate(
                    *parsed.expression, context, talas::expr::EvaluationOptions{parameter.domain});
                if (evaluated.ok() && std::isfinite(evaluated.value.value))
                    return smalltalk::Value::of(evaluated.value.value, unitOf(parameter.domain));
            }
            return smalltalk::Value::expression(source);
        }
        case talas::core::ParameterKind::Resource:
            return smalltalk::Value::string(
                value != nullptr && value->resource.has_value() ? *value->resource : parameter.defaultResource);
        case talas::core::ParameterKind::Text:
            return smalltalk::Value::string(
                value != nullptr && value->text.has_value() ? *value->text : parameter.defaultText);
        case talas::core::ParameterKind::Boolean:
            return smalltalk::Value::of(value != nullptr && value->boolean.has_value()
                    ? *value->boolean
                    : parameter.defaultBoolean.value_or(false));
        case talas::core::ParameterKind::Choice:
            return smalltalk::Value::string(
                value != nullptr && value->choice.has_value() ? *value->choice : parameter.defaultChoice);
        }
        return smalltalk::Value::nil();
    }

    bool write(const talas::core::ParameterDescriptor& parameter, const smalltalk::Value& value, std::string& error)
    {
        if (node_.typeId == "talas.midiVoice" && parameter.id == "polyphony")
        {
            error = "MIDI Voice polyphony is structural and cannot be rewritten after voice-scope materialization";
            return false;
        }
        auto* field = findProjectParameter(node_, parameter.id);
        if (field == nullptr)
        {
            ProjectParameterExpression added;
            added.parameterId = parameter.id;
            node_.parameters.push_back(std::move(added));
            field = &node_.parameters.back();
        }
        const auto isText = value.kind == smalltalk::Value::Kind::String
            || value.kind == smalltalk::Value::Kind::Symbol;
        switch (parameter.kind)
        {
        case talas::core::ParameterKind::Expression:
        {
            const auto text = value.kind == smalltalk::Value::Kind::Number
                    || value.kind == smalltalk::Value::Kind::Expression
                ? smalltalk::fieldText(value)
                : std::nullopt;
            if (!text.has_value() || text->size() > talas::expr::ExpressionLimits::maximumSourceBytes
                || !talas::expr::parse(*text, document_.expressionLanguage, document_.expressionDefinitions).ok())
            {
                error = "The value given " + parameter.id + " is no expression its field can hold";
                return false;
            }
            field->expression = *text;
            break;
        }
        case talas::core::ParameterKind::Resource:
        case talas::core::ParameterKind::Text:
            if (!isText)
            {
                error = parameter.id + " takes a name in quotes";
                return false;
            }
            if (parameter.kind == talas::core::ParameterKind::Resource)
                field->resource = value.text;
            else
                field->text = value.text;
            break;
        case talas::core::ParameterKind::Boolean:
            if (value.kind != smalltalk::Value::Kind::Boolean)
            {
                error = parameter.id + " takes true or false";
                return false;
            }
            field->boolean = value.boolean;
            break;
        case talas::core::ParameterKind::Choice:
            if (!isText || std::ranges::find(parameter.choices, value.text) == parameter.choices.end())
            {
                error = parameter.id + " takes one of its choices";
                return false;
            }
            field->choice = value.text;
            break;
        }
        ++writes;
        return true;
    }

    const ProjectDocument& document_;
    const talas::core::NodeTypeDescriptor& descriptor_;
    ProjectNode& node_;
    double sampleRate_ = 48'000.0;
    std::optional<double> startSeconds_;
    std::optional<double> totalSeconds_;
};
}

ProjectParameterTransformationParseResult
parseProjectParameterTransformation(std::string_view source)
{
    ProjectParameterTransformationParseResult result;
    try
    {
        result.diagnostics.reserve(
            maximumParameterTransformationDiagnostics);
        if (source.size() > maximumParameterTransformationSourceBytes)
        {
            addDiagnostic(
                result,
                ProjectParameterTransformationDiagnosticCode::SourceTooLong,
                "Parameter transformation exceeds the 4096-byte source limit",
                {0, source.size()});
            return result;
        }
        if (!json::isValidUtf8(source))
        {
            addDiagnostic(
                result,
                ProjectParameterTransformationDiagnosticCode::InvalidUtf8,
                "Parameter transformation must be valid UTF-8",
                {0, source.size()});
            return result;
        }

        // A Transformation is Smalltalk; set and scale rules remain for the
        // projects that hold them.
        auto firstLine = std::string_view{};
        for (std::size_t begin = 0; begin < source.size();)
        {
            const auto newline = source.find('\n', begin);
            const auto end = newline == std::string_view::npos ? source.size() : newline;
            firstLine = trimAscii(source.substr(begin, end - begin));
            if (!firstLine.empty() && firstLine != "\r")
                break;
            firstLine = {};
            begin = end + 1;
        }
        if (!firstLine.empty() && !firstLine.starts_with("set ") && !firstLine.starts_with("scale "))
        {
            auto parsed = smalltalk::parse(source);
            if (!parsed.ok())
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::InvalidSyntax,
                    parsed.diagnostic.has_value() ? parsed.diagnostic->message
                                                  : std::string{"The Transformation could not be parsed"},
                    parsed.diagnostic.has_value() ? parsed.diagnostic->range : talas::expr::SourceRange{});
                return result;
            }
            result.program = std::move(parsed.program);
            return result;
        }

        std::size_t lineBegin = 0;
        while (lineBegin <= source.size())
        {
            const auto newline = source.find('\n', lineBegin);
            const auto rawEnd = newline == std::string_view::npos
                ? source.size()
                : newline;
            auto lineEnd = rawEnd;
            if (lineEnd > lineBegin && source[lineEnd - 1] == '\r')
                --lineEnd;
            auto line = source.substr(lineBegin, lineEnd - lineBegin);
            const auto trimmed = trimAscii(line);
            if (!trimmed.empty())
            {
                if (result.rules.size()
                    >= maximumParameterTransformationRules)
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::TooManyRules,
                        "Parameter transformation exceeds the 64-rule limit",
                        {lineBegin, lineEnd - lineBegin});
                    return result;
                }
                ProjectParameterTransformationRule rule;
                rule.sourceRange = {lineBegin, lineEnd - lineBegin};
                if (trimmed.starts_with("set "))
                {
                    const auto body = trimAscii(trimmed.substr(4));
                    const auto equals = body.find('=');
                    if (equals == std::string_view::npos)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::InvalidSyntax,
                            "Expected 'set <parameter-id> = <expression>'",
                            rule.sourceRange);
                    }
                    else
                    {
                        const auto parameterId =
                            trimAscii(body.substr(0, equals));
                        const auto expression =
                            trimAscii(body.substr(equals + 1));
                        if (!validParameterId(parameterId))
                        {
                            addDiagnostic(
                                result,
                                ProjectParameterTransformationDiagnosticCode::InvalidParameterId,
                                "Set rule parameter ID is invalid",
                                rule.sourceRange);
                        }
                        else if (expression.empty()
                            || expression.size()
                                > talas::expr::ExpressionLimits::maximumSourceBytes)
                        {
                            addDiagnostic(
                                result,
                                ProjectParameterTransformationDiagnosticCode::InvalidExpression,
                                "Set rule expression must be non-empty and bounded",
                                rule.sourceRange);
                        }
                        else
                        {
                            rule.kind =
                                ProjectParameterTransformationRuleKind::Set;
                            rule.parameterId.assign(parameterId);
                            rule.operand.assign(expression);
                            result.rules.push_back(std::move(rule));
                        }
                    }
                }
                else if (trimmed.starts_with("scale "))
                {
                    const auto body = trimAscii(trimmed.substr(6));
                    const auto separator = body.find(" by ");
                    if (separator == std::string_view::npos)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::InvalidSyntax,
                            "Expected 'scale <parameter-id> by <finite-scalar>'",
                            rule.sourceRange);
                    }
                    else
                    {
                        const auto parameterId =
                            trimAscii(body.substr(0, separator));
                        const auto scalarText =
                            trimAscii(body.substr(separator + 4));
                        double scalar = 0.0;
                        const auto parsed = talas::fromChars(
                            scalarText.data(),
                            scalarText.data() + scalarText.size(),
                            scalar,
                            std::chars_format::general);
                        if (!validParameterId(parameterId))
                        {
                            addDiagnostic(
                                result,
                                ProjectParameterTransformationDiagnosticCode::InvalidParameterId,
                                "Scale rule parameter ID is invalid",
                                rule.sourceRange);
                        }
                        else if (scalarText.empty()
                            || scalarText.size() > 64
                            || parsed.ec != std::errc{}
                            || parsed.ptr
                                != scalarText.data() + scalarText.size()
                            || !std::isfinite(scalar))
                        {
                            addDiagnostic(
                                result,
                                ProjectParameterTransformationDiagnosticCode::InvalidScale,
                                "Scale rule requires one exact finite binary64 token",
                                rule.sourceRange);
                        }
                        else
                        {
                            rule.kind =
                                ProjectParameterTransformationRuleKind::Scale;
                            rule.parameterId.assign(parameterId);
                            rule.operand.assign(scalarText);
                            result.rules.push_back(std::move(rule));
                        }
                    }
                }
                else
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::InvalidSyntax,
                        "Expected a set or scale rule",
                        rule.sourceRange);
                }
                if (!result.diagnostics.empty())
                    return result;
            }
            if (newline == std::string_view::npos)
                break;
            lineBegin = newline + 1;
        }
    }
    catch (const std::bad_alloc&)
    {
        result.rules.clear();
        result.diagnostics.clear();
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::AllocationFailed;
    }
    catch (const std::length_error&)
    {
        result.rules.clear();
        result.diagnostics.clear();
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::CapacityExceeded;
    }
    return result;
}

bool transformsBeforeExpansion(std::string_view transformation) noexcept
{
    constexpr std::string_view word{"doNotExpandFirst"};
    for (auto at = transformation.find(word); at != std::string_view::npos; at = transformation.find(word, at + 1))
    {
        const auto end = at + word.size();
        const auto isName = [](char character) {
            return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
        };
        if ((at == 0 || !isName(transformation[at - 1])) && (end == transformation.size() || !isName(transformation[end])))
            return true;
    }
    return false;
}

ProjectParameterTransformationResult
prepareProjectParameterTransformers(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const ProjectParameterTransformationClock& clock)
{
    std::size_t traversalRecords = 0;
    return prepareProjectParameterTransformersImpl(
        document,
        registry,
        {},
        traversalRecords,
        &clock);
}

namespace
{
ProjectParameterTransformationResult
prepareProjectParameterTransformersImpl(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    std::span<const std::string> openBoundaryNodeIds,
    std::size_t& traversalRecords,
    const ProjectParameterTransformationClock* clock)
{
    ProjectParameterTransformationResult result;
    try
    {
        result.diagnostics.reserve(
            maximumParameterTransformationDiagnostics);
        std::vector<PreparedTransformer> transformers;
        transformers.reserve(maximumParameterTransformerNodes);
        for (std::size_t nodeIndex = 0;
             nodeIndex < document.graph.nodes.size();
             ++nodeIndex)
        {
            const auto& node = document.graph.nodes[nodeIndex];
            if (node.typeId != "talas.parameterTransformer")
                continue;
            if (transformers.size() >= maximumParameterTransformerNodes)
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::TooManyTransformers,
                    "Project exceeds the 64-ParameterTransformer limit",
                    {},
                    node.instanceId);
                return result;
            }
            const auto* descriptor = registry.findNodeType(node.typeId);
            if (descriptor == nullptr)
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::UnknownTransformerType,
                    "ParameterTransformer descriptor is unavailable",
                    {},
                    node.instanceId);
                return result;
            }
            if (clock != nullptr && clock->beforeExpansion
                && !transformsBeforeExpansion(transformationText(node, *descriptor)))
            {
                continue;
            }
            const auto parsed = parseProjectParameterTransformation(
                transformationText(node, *descriptor));
            if (!parsed.ok())
            {
                result.terminalFailure = parsed.terminalFailure;
                result.truncated = parsed.truncated;
                for (auto diagnostic : parsed.diagnostics)
                {
                    diagnostic.transformerNodeId = node.instanceId;
                    if (result.diagnostics.size()
                        >= maximumParameterTransformationDiagnostics)
                    {
                        result.truncated = true;
                        break;
                    }
                    result.diagnostics.push_back(std::move(diagnostic));
                }
                return result;
            }
            if (!parsed.rules.empty() || parsed.program != nullptr)
            {
                transformers.push_back({
                    nodeIndex,
                    parsed.rules,
                    parsed.program,
                    {},
                    0,
                    false,
                });
            }
        }

        if (transformers.empty())
        {
            result.graph = document.graph;
            return result;
        }
        if (document.graph.nodes.size()
                > maximumParameterTransformationTraversalRecords
            || document.graph.connections.size()
                > maximumParameterTransformationTraversalRecords
                    - document.graph.nodes.size())
        {
            addDiagnostic(
                result,
                ProjectParameterTransformationDiagnosticCode::GraphTooLarge,
                "Parameter transformation graph exceeds the fixed traversal budget");
            return result;
        }

        std::unordered_map<std::string_view, std::size_t> nodeIndices;
        nodeIndices.reserve(document.graph.nodes.size());
        for (std::size_t index = 0; index < document.graph.nodes.size(); ++index)
        {
            if (!nodeIndices.emplace(
                    document.graph.nodes[index].instanceId,
                    index).second)
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    "Parameter transformation requires unique node identities");
                return result;
            }
        }
        std::vector<std::size_t> openBoundaryNodeIndices;
        openBoundaryNodeIndices.reserve(openBoundaryNodeIds.size());
        for (const auto& nodeId : openBoundaryNodeIds)
        {
            const auto node = nodeIndices.find(nodeId);
            if (node == nodeIndices.end())
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    "Parameter transformation open-boundary marker is unavailable");
                return result;
            }
            openBoundaryNodeIndices.push_back(node->second);
        }
        std::vector<std::vector<std::size_t>> incoming(
            document.graph.nodes.size());
        std::vector<std::vector<std::size_t>> outgoing(
            document.graph.nodes.size());
        for (std::size_t index = 0;
             index < document.graph.connections.size();
             ++index)
        {
            const auto& connection = document.graph.connections[index];
            const auto from = nodeIndices.find(connection.fromNode);
            const auto to = nodeIndices.find(connection.toNode);
            if (from == nodeIndices.end() || to == nodeIndices.end())
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    "Parameter transformation graph contains an unknown connection endpoint");
                return result;
            }
            incoming[to->second].push_back(index);
            outgoing[from->second].push_back(index);
        }

        for (auto& transformer : transformers)
        {
            const auto& transformerNode =
                document.graph.nodes[transformer.nodeIndex];
            std::optional<std::size_t> inputSource;
            for (const auto connectionIndex : incoming[transformer.nodeIndex])
            {
                const auto& connection =
                    document.graph.connections[connectionIndex];
                if (connection.toPort != "audioIn")
                    continue;
                if (inputSource.has_value())
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                        "ParameterTransformer audioIn resolves to multiple sources",
                        {},
                        transformerNode.instanceId);
                    return result;
                }
                inputSource = nodeIndices.at(connection.fromNode);
            }
            if (!inputSource.has_value())
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::MissingInput,
                    "ParameterTransformer requires one exact audioIn source",
                    {},
                    transformerNode.instanceId);
                return result;
            }
            transformer.closure.assign(
                document.graph.nodes.size(), 0);
            std::vector<std::size_t> stack{*inputSource};
            while (!stack.empty())
            {
                const auto nodeIndex = stack.back();
                stack.pop_back();
                if (transformer.closure[nodeIndex] != 0)
                    continue;
                transformer.closure[nodeIndex] = 1;
                ++transformer.closureSize;
                if (++traversalRecords
                    > maximumParameterTransformationTraversalRecords)
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
                        "Parameter transformation traversal exceeded its fixed budget",
                        {},
                        transformerNode.instanceId);
                    return result;
                }
                for (const auto connectionIndex : incoming[nodeIndex])
                {
                    if (++traversalRecords
                        > maximumParameterTransformationTraversalRecords)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
                            "Parameter transformation traversal exceeded its fixed budget",
                            {},
                            transformerNode.instanceId);
                        return result;
                    }
                    const auto& connection =
                        document.graph.connections[connectionIndex];
                    stack.push_back(nodeIndices.at(connection.fromNode));
                }
            }
            transformer.deferredOpenBoundary = std::ranges::any_of(
                openBoundaryNodeIndices,
                [&](const auto boundaryNodeIndex) {
                    return transformer.closure[boundaryNodeIndex] != 0;
                });
            std::vector<std::size_t> closureIndegrees(
                document.graph.nodes.size(), 0);
            std::vector<std::size_t> ready;
            ready.reserve(transformer.closureSize);
            for (std::size_t nodeIndex = 0;
                 nodeIndex < transformer.closure.size();
                 ++nodeIndex)
            {
                if (transformer.closure[nodeIndex] == 0)
                    continue;
                for (const auto connectionIndex : incoming[nodeIndex])
                {
                    const auto source = nodeIndices.at(
                        document.graph.connections[connectionIndex].fromNode);
                    if (transformer.closure[source] != 0)
                        ++closureIndegrees[nodeIndex];
                }
                if (closureIndegrees[nodeIndex] == 0)
                    ready.push_back(nodeIndex);
            }
            std::size_t visitedClosureNodes = 0;
            while (!ready.empty())
            {
                const auto nodeIndex = ready.back();
                ready.pop_back();
                ++visitedClosureNodes;
                for (const auto connectionIndex : outgoing[nodeIndex])
                {
                    const auto destination = nodeIndices.at(
                        document.graph.connections[connectionIndex].toNode);
                    if (transformer.closure[destination] == 0)
                        continue;
                    if (--closureIndegrees[destination] == 0)
                        ready.push_back(destination);
                }
            }
            if (visitedClosureNodes != transformer.closureSize)
            {
                addDiagnostic(
                    result,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    "ParameterTransformer upstream closure contains a cycle",
                    {},
                    transformerNode.instanceId);
                return result;
            }
            for (std::size_t nodeIndex = 0;
                 nodeIndex < transformer.closure.size();
                 ++nodeIndex)
            {
                if (transformer.closure[nodeIndex] == 0)
                    continue;
                for (const auto connectionIndex : outgoing[nodeIndex])
                {
                    const auto& connection =
                        document.graph.connections[connectionIndex];
                    const auto destination =
                        nodeIndices.at(connection.toNode);
                    if (destination != transformer.nodeIndex
                        && transformer.closure[destination] == 0)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::SharedUpstreamBranch,
                            "ParameterTransformer input closure is shared with an outside consumer",
                            {},
                            transformerNode.instanceId,
                            0,
                            document.graph.nodes[nodeIndex].instanceId);
                        return result;
                    }
                }
            }
        }

        std::ranges::sort(transformers, [&](const auto& lhs, const auto& rhs) {
            if (lhs.closureSize != rhs.closureSize)
                return lhs.closureSize < rhs.closureSize;
            return document.graph.nodes[lhs.nodeIndex].instanceId
                < document.graph.nodes[rhs.nodeIndex].instanceId;
        });

        ProjectGraph candidate = document.graph;
        for (const auto& transformer : transformers)
        {
            const auto& transformerNode =
                candidate.nodes[transformer.nodeIndex];
            if (transformer.deferredOpenBoundary)
                continue;
            for (std::size_t ruleIndex = 0;
                 ruleIndex < transformer.rules.size();
                 ++ruleIndex)
            {
                const auto& rule = transformer.rules[ruleIndex];
                std::size_t matches = 0;
                std::size_t passedOver = 0;
                for (std::size_t nodeIndex = 0;
                     nodeIndex < candidate.nodes.size();
                     ++nodeIndex)
                {
                    if (transformer.closure[nodeIndex] == 0)
                        continue;
                    auto& node = candidate.nodes[nodeIndex];
                    if (clock != nullptr && clock->transforms
                        && !clock->transforms(transformerNode.instanceId, node))
                    {
                        ++passedOver;
                        continue;
                    }
                    const auto described = node.soundDefinitionId.has_value()
                        ? describeProjectNode(document, node, registry)
                        : std::optional<talas::core::NodeTypeDescriptor>{};
                    const auto* descriptor = described.has_value()
                        ? &*described
                        : registry.findNodeType(node.typeId);
                    if (descriptor == nullptr)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                            "ParameterTransformer upstream closure contains an unavailable node type",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                    const auto* parameterDescriptor =
                        findDescriptorParameter(*descriptor, rule.parameterId);
                    if (parameterDescriptor == nullptr)
                        continue;
                    if (node.typeId == "talas.midiVoice"
                        && rule.parameterId == "polyphony")
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::UnsupportedParameter,
                            "MIDI Voice polyphony is structural and cannot be rewritten after voice-scope materialization",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                    if (parameterDescriptor->kind
                        != talas::core::ParameterKind::Expression)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::UnsupportedParameter,
                            "Parameter transformation can modify only Expression parameters",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                    auto* parameter =
                        findProjectParameter(node, rule.parameterId);
                    const auto current = parameter == nullptr
                        ? std::string_view{parameterDescriptor->defaultExpression}
                        : (parameter->expression.has_value()
                            ? std::string_view{*parameter->expression}
                            : std::string_view{});
                    if (current.empty())
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::UnsupportedParameter,
                            "Matched Expression parameter has no symbolic source",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                    std::string rewritten;
                    if (rule.kind
                        == ProjectParameterTransformationRuleKind::Set)
                    {
                        rewritten = rule.operand;
                    }
                    else
                    {
                        constexpr std::size_t wrapperBytes = 7;
                        if (current.size()
                                > talas::expr::ExpressionLimits::maximumSourceBytes
                                    - wrapperBytes
                            || rule.operand.size()
                                > talas::expr::ExpressionLimits::maximumSourceBytes
                                    - wrapperBytes - current.size())
                        {
                            addDiagnostic(
                                result,
                                ProjectParameterTransformationDiagnosticCode::RewrittenExpressionTooLong,
                                "Scaled expression exceeds the fixed source limit",
                                rule.sourceRange,
                                transformerNode.instanceId,
                                ruleIndex,
                                node.instanceId,
                                rule.parameterId);
                            return result;
                        }
                        rewritten.reserve(
                            current.size() + rule.operand.size()
                            + wrapperBytes);
                        rewritten += '(';
                        rewritten += current;
                        rewritten += ") * (";
                        rewritten += rule.operand;
                        rewritten += ')';
                    }
                    const auto expression = talas::expr::parse(
                        rewritten,
                        document.expressionLanguage,
                        document.expressionDefinitions);
                    if (!expression.ok())
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::InvalidExpression,
                            "Rewritten parameter expression is invalid",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                    if (parameter == nullptr)
                    {
                        node.parameters.push_back({
                            rule.parameterId,
                            std::move(rewritten),
                        });
                    }
                    else
                    {
                        parameter->expression = std::move(rewritten);
                    }
                    ++matches;
                    if (++result.transformedParameters
                        > maximumParameterTransformationWrites)
                    {
                        addDiagnostic(
                            result,
                            ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
                            "Parameter transformation exceeded the 4096-write limit",
                            rule.sourceRange,
                            transformerNode.instanceId,
                            ruleIndex,
                            node.instanceId,
                            rule.parameterId);
                        return result;
                    }
                }
                if (matches == 0 && passedOver == 0)
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::UnmatchedParameter,
                        "Parameter transformation rule matched no upstream Expression parameter",
                        rule.sourceRange,
                        transformerNode.instanceId,
                        ruleIndex,
                        {},
                        rule.parameterId);
                    return result;
                }
            }
            // A Smalltalk Transformation runs once for each Sound of the
            // Input, before it is compiled; the
            // editor's preflight only parses it.
            if (transformer.program == nullptr || clock == nullptr)
                continue;
            const auto transformerId = transformerNode.instanceId;
            const auto inputConnection = std::ranges::find_if(candidate.connections, [&](const auto& connection) {
                return connection.toNode == transformerId && connection.toPort == "audioIn";
            });
            const auto inputId = inputConnection == candidate.connections.end()
                ? std::string{}
                : inputConnection->fromNode;
            const auto totalSeconds = clock->durationSeconds && !inputId.empty()
                ? clock->durationSeconds(candidate, inputId)
                : std::optional<double>{};
            // The Input first, then each Sound after the Sounds it feeds, so
            // that a Sound skipped as a level's subsound is known before its
            // inputs. A Sound is transformed when one
            // of the Sounds it feeds goes on to its subsounds, or when it
            // feeds none in the closure.
            std::vector<std::size_t> consumers(candidate.nodes.size(), 0);
            std::vector<std::vector<std::size_t>> producers(candidate.nodes.size());
            {
                std::map<std::string, std::size_t, std::less<>> indexOf;
                for (std::size_t index = 0; index < candidate.nodes.size(); ++index)
                    indexOf.emplace(candidate.nodes[index].instanceId, index);
                for (const auto& connection : candidate.connections)
                {
                    const auto from = indexOf.find(connection.fromNode);
                    const auto to = indexOf.find(connection.toNode);
                    if (from == indexOf.end() || to == indexOf.end() || transformer.closure[from->second] == 0
                        || transformer.closure[to->second] == 0)
                    {
                        continue;
                    }
                    ++consumers[from->second];
                    producers[to->second].push_back(from->second);
                }
            }
            std::vector<std::size_t> order;
            std::vector<std::size_t> pending(consumers);
            for (std::size_t index = 0; index < candidate.nodes.size(); ++index)
            {
                if (transformer.closure[index] != 0 && pending[index] == 0)
                    order.push_back(index);
            }
            std::vector<char> reached(candidate.nodes.size(), 0);
            for (std::size_t index = 0; index < order.size(); ++index)
            {
                for (const auto producer : producers[order[index]])
                {
                    if (--pending[producer] == 0)
                        order.push_back(producer);
                }
            }
            for (const auto nodeIndex : order)
            {
                if (consumers[nodeIndex] != 0 && reached[nodeIndex] == 0)
                    continue;
                auto& node = candidate.nodes[nodeIndex];
                if (clock->transforms && !clock->transforms(transformerId, node))
                {
                    for (const auto producer : producers[nodeIndex])
                        reached[producer] = 1;
                    continue;
                }
                const auto described = node.soundDefinitionId.has_value()
                    ? describeProjectNode(document, node, registry)
                    : std::optional<talas::core::NodeTypeDescriptor>{};
                const auto* descriptor = described.has_value()
                    ? &*described
                    : registry.findNodeType(node.typeId);
                if (descriptor == nullptr)
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                        "ParameterTransformer upstream closure contains an unavailable node type",
                        {},
                        transformerId,
                        0,
                        node.instanceId);
                    return result;
                }
                const auto startSeconds = clock->startSeconds && !inputId.empty()
                    ? clock->startSeconds(candidate, node.instanceId, inputId)
                    : std::optional<double>{};
                auto transformed = node;
                TransformedSound sound(
                    document, *descriptor, transformed, clock->sampleRate, startSeconds, totalSeconds);
                const auto ran = smalltalk::run(*transformer.program, sound);
                if (!ran.ok())
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::InvalidExpression,
                        ran.diagnostic->message,
                        ran.diagnostic->range,
                        transformerId,
                        0,
                        node.instanceId);
                    return result;
                }
                if (sound.transformThisSound)
                {
                    node = std::move(transformed);
                    result.transformedParameters += sound.writes;
                }
                if (sound.transformSubSounds)
                {
                    for (const auto producer : producers[nodeIndex])
                        reached[producer] = 1;
                }
                if (result.transformedParameters > maximumParameterTransformationWrites)
                {
                    addDiagnostic(
                        result,
                        ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
                        "Parameter transformation exceeded the 4096-write limit",
                        {},
                        transformerId,
                        0,
                        node.instanceId);
                    return result;
                }
            }
        }

        for (const auto& transformer : transformers)
        {
            if (clock != nullptr && clock->keepsTransformations)
                break;
            auto& node = candidate.nodes[transformer.nodeIndex];
            if (auto* parameter =
                    findProjectParameter(node, "transformation");
                parameter != nullptr)
            {
                parameter->text = std::string{};
            }
        }
        result.graph = std::move(candidate);
    }
    catch (const std::bad_alloc&)
    {
        result.graph.reset();
        result.diagnostics.clear();
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::AllocationFailed;
    }
    catch (const std::length_error&)
    {
        result.graph.reset();
        result.diagnostics.clear();
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::CapacityExceeded;
    }
    return result;
}
}

ProjectParameterTransformationResult
prepareAuthoredProjectParameterTransformers(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    ProjectParameterTransformationResult result;
    try
    {
        const auto hasNonblankTransformer = [](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [](const auto& node) {
                if (node.typeId != "talas.parameterTransformer")
                    return false;
                const auto parameter = std::ranges::find(
                    node.parameters,
                    std::string_view{"transformation"},
                    &ProjectParameterExpression::parameterId);
                return parameter != node.parameters.end()
                    && parameter->text.has_value()
                    && !parameter->text->empty();
            });
        };
        const auto validateTransformerSources = [&](const ProjectGraph& graph) {
            ProjectParameterTransformationResult validated;
            for (const auto& node : graph.nodes)
            {
                if (node.typeId != "talas.parameterTransformer")
                    continue;
                const auto* descriptor = registry.findNodeType(node.typeId);
                if (descriptor == nullptr)
                    continue;
                const auto parsed = parseProjectParameterTransformation(
                    transformationText(node, *descriptor));
                if (!parsed.ok())
                {
                    validated.terminalFailure = parsed.terminalFailure;
                    validated.truncated = parsed.truncated;
                    for (auto diagnostic : parsed.diagnostics)
                    {
                        diagnostic.transformerNodeId = node.instanceId;
                        validated.diagnostics.push_back(
                            std::move(diagnostic));
                    }
                    return validated;
                }
                for (std::size_t ruleIndex = 0;
                     ruleIndex < parsed.rules.size();
                     ++ruleIndex)
                {
                    const auto& rule = parsed.rules[ruleIndex];
                    if (rule.kind
                        != ProjectParameterTransformationRuleKind::Set)
                    {
                        continue;
                    }
                    const auto expression = talas::expr::parse(
                        rule.operand,
                        document.expressionLanguage,
                        document.expressionDefinitions);
                    if (!expression.ok())
                    {
                        addDiagnostic(
                            validated,
                            ProjectParameterTransformationDiagnosticCode::InvalidExpression,
                            "Set rule expression is invalid",
                            rule.sourceRange,
                            node.instanceId,
                            ruleIndex,
                            {},
                            rule.parameterId);
                        return validated;
                    }
                }
            }
            validated.graph = ProjectGraph{};
            return validated;
        };
        if (!hasNonblankTransformer(document.graph)
            && std::ranges::none_of(
                document.soundDefinitions,
                [&](const auto& definition) {
                    return hasNonblankTransformer(definition.graph);
                }))
        {
            result.graph = document.graph;
            return result;
        }
        auto sourceValidation = validateTransformerSources(document.graph);
        if (!sourceValidation.ok())
            return sourceValidation;
        for (const auto& definition : document.soundDefinitions)
        {
            sourceValidation = validateTransformerSources(definition.graph);
            if (!sourceValidation.ok())
                return sourceValidation;
        }
        std::unordered_map<std::string_view, std::size_t> definitionIndices;
        definitionIndices.reserve(document.soundDefinitions.size());
        for (std::size_t index = 0;
             index < document.soundDefinitions.size();
             ++index)
        {
            definitionIndices.emplace(
                document.soundDefinitions[index].definitionId,
                index);
        }
        std::vector<std::uint8_t> definitionStates(
            document.soundDefinitions.size(), 0);
        std::vector<std::uint8_t> definitionContainsTransformer(
            document.soundDefinitions.size(), 0);
        const auto closureContainsTransformer = [&]<typename Self>(
                                                    std::size_t index,
                                                    Self&& self) -> bool {
            if (definitionStates[index] == 2)
                return definitionContainsTransformer[index] != 0;
            if (definitionStates[index] == 1)
                return false;
            definitionStates[index] = 1;
            const auto& definition = document.soundDefinitions[index];
            bool contains = hasNonblankTransformer(definition.graph);
            for (const auto& node : definition.graph.nodes)
            {
                if (contains || !node.soundDefinitionId.has_value())
                    continue;
                const auto nested = definitionIndices.find(
                    *node.soundDefinitionId);
                if (nested != definitionIndices.end())
                    contains = self(nested->second, self);
            }
            definitionContainsTransformer[index] = contains ? 1 : 0;
            definitionStates[index] = 2;
            return contains;
        };
        for (std::size_t index = 0;
             index < document.soundDefinitions.size();
             ++index)
        {
            static_cast<void>(closureContainsTransformer(
                index,
                closureContainsTransformer));
        }
        std::size_t traversalRecords = 0;
        std::size_t expandedGraphRecords = 0;
        const auto prepareExpanded = [&](
                                         const ProjectDocument& source,
                                         ProjectSoundExpansionResult expanded) {
            ProjectParameterTransformationResult prepared;
            if (!expanded.ok())
            {
                addDiagnostic(
                    prepared,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    expanded.validation.errors.empty()
                        ? "Reusable Sound expansion failed before parameter transformation validation"
                        : expanded.validation.errors.front().message,
                    {},
                    expanded.validation.errors.empty()
                        ? std::string{}
                        : expanded.validation.errors.front().nodeId);
                return prepared;
            }
            const auto midiVoiceIslands = discoverProjectMidiVoiceIslands(
                expanded.document,
                expanded.manifest,
                {});
            if (!midiVoiceIslands.ok())
            {
                addDiagnostic(
                    prepared,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    midiVoiceIslands.error,
                    {},
                    midiVoiceIslands.errorNodeId);
                return prepared;
            }
            const auto materialized = materializeProjectSoundVariableBranches(
                source,
                expanded,
                midiVoiceIslands.islands);
            if (!materialized.ok())
            {
                addDiagnostic(
                    prepared,
                    ProjectParameterTransformationDiagnosticCode::InvalidGraph,
                    materialized.errors.empty()
                        ? "Sound-variable branch materialization failed before parameter transformation validation"
                        : materialized.errors.front().message,
                    {},
                    materialized.errors.empty()
                        ? std::string{}
                        : materialized.errors.front().nodeId);
                return prepared;
            }
            const auto graphRecords = expanded.document.graph.nodes.size()
                + expanded.document.graph.connections.size();
            if (graphRecords
                > maximumParameterTransformationTraversalRecords
                    - expandedGraphRecords)
            {
                addDiagnostic(
                    prepared,
                    ProjectParameterTransformationDiagnosticCode::CapacityExceeded,
                    "Authored parameter transformation expansion exceeded its fixed aggregate budget");
                return prepared;
            }
            expandedGraphRecords += graphRecords;
            return prepareProjectParameterTransformersImpl(
                expanded.document,
                registry,
                expanded.preparationOpenBoundaryNodeIds,
                traversalRecords,
                nullptr);
        };

        auto rootPrepared = prepareExpanded(
            document,
            expandProjectSounds(document, registry));
        if (!rootPrepared.ok())
            return rootPrepared;
        for (std::size_t definitionIndex = 0;
             definitionIndex < document.soundDefinitions.size();
             ++definitionIndex)
        {
            if (definitionContainsTransformer[definitionIndex] == 0)
                continue;
            const auto& definition =
                document.soundDefinitions[definitionIndex];
            const auto definitionPrepared = prepareExpanded(
                document,
                expandProjectSoundDefinitionForPreparation(
                    document,
                    definition.definitionId,
                    registry));
            if (!definitionPrepared.ok())
                return definitionPrepared;
        }
        return rootPrepared;
    }
    catch (const std::bad_alloc&)
    {
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::AllocationFailed;
    }
    catch (const std::length_error&)
    {
        result.terminalFailure =
            ProjectParameterTransformationDiagnosticCode::CapacityExceeded;
    }
    return result;
}
}
