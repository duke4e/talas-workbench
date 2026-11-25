#pragma once

#include "talas/runtime/Runtime.h"

#include <algorithm>

namespace talas::runtime::voice_expression_authority
{
// Compiled expressions retain their language after specialization. Inspect that
// provenance as well as settings so a reconstructed public plan cannot erase
// the compiler-authority requirement by downgrading its language setting.
[[nodiscard]] inline bool requiresCompilerSeal(
    const RuntimePlan& plan, const RenderSettings& settings)
{
    const auto isVoiceExpression = [](const auto& expression) {
        return expression.has_value()
            && talas::expr::supportsVoiceNumberSpecialization(expression->languageVersion());
    };
    return talas::expr::supportsVoiceNumberSpecialization(settings.expressionLanguage)
        || std::ranges::any_of(plan.steps, [&](const CompileStep& step) {
            return std::ranges::any_of(step.parameters, [&](const ParameterValue& parameter) {
                return isVoiceExpression(parameter.expression);
            });
        })
        || std::ranges::any_of(settings.eventValues, [&](const EventValueBinding& binding) {
            return isVoiceExpression(binding.projectionExpression);
        });
}
}
