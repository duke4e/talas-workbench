#pragma once

#include "talas/expr/Expression.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project::smalltalk
{
class Program;
}

namespace talas::project
{
inline constexpr std::size_t maximumParameterTransformationSourceBytes = 4096;
inline constexpr std::size_t maximumParameterTransformationRules = 64;
inline constexpr std::size_t maximumParameterTransformerNodes = 64;
inline constexpr std::size_t maximumParameterTransformationTraversalRecords = 65536;
inline constexpr std::size_t maximumParameterTransformationWrites = 4096;
inline constexpr std::size_t maximumParameterTransformationDiagnostics = 64;
inline constexpr std::size_t maximumParameterTransformationIdentifierBytes = 128;

enum class ProjectParameterTransformationRuleKind
{
    Set,
    Scale,
};

struct ProjectParameterTransformationRule
{
    ProjectParameterTransformationRuleKind kind =
        ProjectParameterTransformationRuleKind::Set;
    std::string parameterId;
    std::string operand;
    talas::expr::SourceRange sourceRange;

    bool operator==(const ProjectParameterTransformationRule&) const = default;
};

enum class ProjectParameterTransformationDiagnosticCode
{
    InvalidUtf8,
    SourceTooLong,
    TooManyRules,
    InvalidSyntax,
    InvalidParameterId,
    InvalidScale,
    InvalidExpression,
    TooManyTransformers,
    GraphTooLarge,
    InvalidGraph,
    UnknownTransformerType,
    MissingInput,
    SharedUpstreamBranch,
    UnmatchedParameter,
    UnsupportedParameter,
    RewrittenExpressionTooLong,
    CapacityExceeded,
    AllocationFailed,
};

struct ProjectParameterTransformationDiagnostic
{
    ProjectParameterTransformationDiagnosticCode code =
        ProjectParameterTransformationDiagnosticCode::InvalidSyntax;
    std::string transformerNodeId;
    std::string targetNodeId;
    std::string parameterId;
    std::size_t ruleIndex = 0;
    talas::expr::SourceRange sourceRange;
    std::string message;
};

struct ProjectParameterTransformationParseResult
{
    std::vector<ProjectParameterTransformationRule> rules;
    // A Transformation in Smalltalk,
    // run on each Sound of the Input in place of rules.
    std::shared_ptr<const smalltalk::Program> program;
    std::vector<ProjectParameterTransformationDiagnostic> diagnostics;
    std::optional<ProjectParameterTransformationDiagnosticCode>
        terminalFailure;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !terminalFailure.has_value()
            && !truncated;
    }
};

struct ProjectParameterTransformationResult
{
    std::optional<ProjectGraph> graph;
    std::vector<ProjectParameterTransformationDiagnostic> diagnostics;
    std::optional<ProjectParameterTransformationDiagnosticCode>
        terminalFailure;
    bool truncated = false;
    std::size_t transformedParameters = 0;

    [[nodiscard]] bool ok() const noexcept
    {
        return graph.has_value() && diagnostics.empty()
            && !terminalFailure.has_value() && !truncated;
    }
};

[[nodiscard]] ProjectParameterTransformationParseResult
parseProjectParameterTransformation(std::string_view source);

// When the Sounds inside a ParameterTransformer play, for a Transformation's
// time and totalDuration, and the sample rate its fields
// are read at: a Sound's start after the transformer's Input starts, and a
// Sound's duration, in seconds, or nothing where they are not fixed.
struct ProjectParameterTransformationClock
{
    double sampleRate = 48'000.0;
    std::function<std::optional<double>(
        const ProjectGraph& graph, std::string_view nodeId, std::string_view inputId)>
        startSeconds;
    std::function<std::optional<double>(const ProjectGraph& graph, std::string_view nodeId)>
        durationSeconds;
    // Only the Transformations that send snd doNotExpandFirst, which apply
    // before the Sounds they transform expand.
    bool beforeExpansion = false;
    // When set, only the Sounds it accepts for a ParameterTransformer are
    // transformed by it; the others are passed over, their subsounds still
    // reached (an instance whose class does not expand first is transformed
    // before its Sounds expand).
    std::function<bool(std::string_view transformerId, const ProjectNode& node)> transforms;
    // Leave the Transformations in place, for a later pass.
    bool keepsTransformations = false;
};

// Whether a Transformation sends snd doNotExpandFirst.
[[nodiscard]] bool transformsBeforeExpansion(std::string_view transformation) noexcept;

// Produces a compile-only graph candidate. The authored document is never
// mutated. On success every nonblank ParameterTransformer body in the
// candidate is cleared so the existing offline/realtime operation remains a
// pure audio passthrough.
[[nodiscard]] ProjectParameterTransformationResult
prepareProjectParameterTransformers(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const ProjectParameterTransformationClock& clock = {});

// Runs the same structural planner against the authored document after the
// reusable-Sound and Sound-variable expansion boundary used by compilation.
// This is the shared editor/persistence preflight; it never mutates the
// authored document.
[[nodiscard]] ProjectParameterTransformationResult
prepareAuthoredProjectParameterTransformers(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry);
}
