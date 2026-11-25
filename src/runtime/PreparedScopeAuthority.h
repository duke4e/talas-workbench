#pragma once

#include "talas/runtime/Runtime.h"

#include <optional>
#include <span>

namespace talas::runtime::prepared_scope
{
// Processing owns buffers and voice-local state. Structural identifies the
// original Sound instance; events identifies the expression's event context.
// MIDI lowering may clone processing into a sibling of the original Sound.
struct Identity
{
    RuntimeScopeId processing;
    RuntimeScopeId structural;
    RuntimeScopeId events;

    [[nodiscard]] static Identity fromStep(const CompileStep& step) noexcept
    {
        return {step.scopeId, step.structuralScopeId, step.eventValueScopeId};
    }
    [[nodiscard]] bool isLocal() const noexcept
    {
        return processing == structural && processing == events;
    }
};

// Membership is an identity query, not execution admission or a replacement
// for validation of complete voice plans. Count ambiguity instead of selecting
// the first match. Subtraction follows bounds checks, including forged ranges.
[[nodiscard]] inline std::size_t midiVoiceMembershipCount(
    const RuntimePlan& plan, std::size_t stepIndex) noexcept
{
    if (stepIndex >= plan.steps.size()) return 0;
    std::size_t count = 0;
    for (const auto& region : plan.midiVoices)
        for (const auto& voice : region.voices)
            if (voice.scopeId == plan.steps[stepIndex].scopeId
                && voice.firstStepIndex <= stepIndex
                && voice.stepCount <= plan.steps.size() - voice.firstStepIndex
                && stepIndex - voice.firstStepIndex < voice.stepCount)
                ++count;
    return count;
}

// Reflexive lookup under the caller's existing prepared-table authority.
// This does not establish that a scope exists or validate the entire tree.
[[nodiscard]] inline bool isScopeOrAncestor(
    std::span<const talas::project::PreparedProjectVariableScope> scopes,
    RuntimeScopeId descendant, RuntimeScopeId candidate) noexcept
{
    auto current = descendant;
    for (std::size_t depth = 0; depth <= scopes.size(); ++depth)
    {
        if (current == candidate) return true;
        if (current == globalRuntimeScope) return false;
        const talas::project::PreparedProjectVariableScope* match = nullptr;
        for (const auto& scope : scopes)
        {
            if (scope.scopeOrdinal != current) continue;
            if (match != nullptr) return false;
            match = &scope;
        }
        if (match == nullptr || match->parentScopeOrdinal == current) return false;
        current = match->parentScopeOrdinal;
    }
    return false;
}

// Preparation-only borrowed view. The caller supplies its private compiler
// plan only after verifying the matching settings/table authority. A public
// reconstructed plan is never its own compiler authority. The original plan
// must outlive this view; it is not retained by a processing checkpoint.
class VoiceSources
{
public:
    [[nodiscard]] static VoiceSources prepare(
        const RuntimePlan& plan, const RuntimePlan* compilerPlan) noexcept
    {
        return VoiceSources{compilerPlan != nullptr && plan == *compilerPlan
            ? compilerPlan : nullptr};
    }
    [[nodiscard]] std::optional<RuntimeScopeId> sourceForStep(std::size_t stepIndex) const noexcept
    {
        if (compilerPlan_ == nullptr
            || midiVoiceMembershipCount(*compilerPlan_, stepIndex) != 1) return std::nullopt;
        return Identity::fromStep(compilerPlan_->steps[stepIndex]).structural;
    }

    [[nodiscard]] bool permitsProjectedScope(
        std::size_t stepIndex,
        RuntimeScopeId candidateScope,
        std::span<const talas::project::PreparedProjectVariableScope> scopes) const noexcept
    {
        if (compilerPlan_ == nullptr
            || stepIndex >= compilerPlan_->steps.size()
            || midiVoiceMembershipCount(*compilerPlan_, stepIndex) != 1)
            return false;

        const auto& step = compilerPlan_->steps[stepIndex];
        if (candidateScope == step.scopeId
            || !isScopeOrAncestor(scopes, candidateScope, step.scopeId))
            return false;

        return step.eventValueScopeId == candidateScope
            || std::ranges::any_of(
                step.parameters,
                [&](const ParameterValue& parameter) {
                    return parameter.variableScopeId == candidateScope;
                });
    }

private:
    explicit VoiceSources(const RuntimePlan* compilerPlan) noexcept : compilerPlan_(compilerPlan) {}
    const RuntimePlan* compilerPlan_;
};

[[nodiscard]] inline bool isProcessingOrSourceAncestor(
    std::span<const talas::project::PreparedProjectVariableScope> scopes,
    RuntimeScopeId processing, std::optional<RuntimeScopeId> compilerVoiceSource,
    RuntimeScopeId candidate) noexcept
{
    return isScopeOrAncestor(scopes, processing, candidate)
        || (compilerVoiceSource.has_value()
            && isScopeOrAncestor(scopes, *compilerVoiceSource, candidate));
}
}
