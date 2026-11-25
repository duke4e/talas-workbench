#pragma once

#include "talas/project/ProjectDocument.h"
#include "talas/core/NodeRegistry.h"

#include <cstddef>
#include <string>
#include <vector>

namespace talas::project
{
inline constexpr std::size_t maximumProjectExpressionDiagnostics = 64;

struct ProjectExpressionDiagnostic
{
    std::string path;
    talas::expr::Diagnostic::Code code =
        talas::expr::Diagnostic::Code::UnexpectedToken;
    talas::expr::SourceRange range;
    std::string message;
};

struct ProjectExpressionAuditResult
{
    std::vector<ProjectExpressionDiagnostic> diagnostics;
    bool truncated = false;

    [[nodiscard]] bool ok() const noexcept
    {
        return diagnostics.empty() && !truncated;
    }
};

struct ProjectVariableReferencePaths
{
    std::vector<std::string> paths;
    bool truncated = false;
};

[[nodiscard]] ProjectExpressionAuditResult auditProjectExpressions(
    const ProjectDocument& document,
    talas::expr::ExpressionLanguageVersion language);

// Includes effective built-in descriptor defaults that are not materialized
// as project overrides. This is the authority used before changing a live
// project's expression language.
[[nodiscard]] ProjectExpressionAuditResult auditProjectExpressions(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::ExpressionLanguageVersion language);

// Audits a change to `language` that must also keep every expression's
// meaning under `previous`: Val sends binary operators from left to
// right, so 1 - 0.5 * 2 would change unless parenthesized.
[[nodiscard]] ProjectExpressionAuditResult auditProjectExpressionsKeepingMeaning(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    talas::expr::ExpressionLanguageVersion language,
    talas::expr::ExpressionLanguageVersion previous);

// Validates one authored parameter against the exact owning descriptor and
// node runtime contract. This keeps ProjectEditor authoring on the same
// language/domain authority as full-project staging and upgrade audits.
[[nodiscard]] ProjectExpressionAuditResult auditProjectParameterExpression(
    const ProjectDocument& document,
    const ProjectNode& node,
    const ProjectParameterExpression& value,
    const talas::core::ParameterDescriptor& descriptor,
    talas::expr::ExpressionLanguageVersion language,
    std::string path = "$.parameter");

[[nodiscard]] ProjectVariableReferencePaths findProjectVariableReferencePaths(
    const ProjectDocument& document,
    const ProjectVariableOwner& owner,
    std::string_view variableId);
}
