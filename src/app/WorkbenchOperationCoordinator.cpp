#include "talas/app/WorkbenchOperationCoordinator.h"

#include <algorithm>
#include <cmath>

namespace talas::app
{
namespace
{
[[nodiscard]] bool terminal(WorkbenchOperationPhase phase) noexcept
{
    return phase == WorkbenchOperationPhase::Idle
        || phase == WorkbenchOperationPhase::Succeeded
        || phase == WorkbenchOperationPhase::Cancelled
        || phase == WorkbenchOperationPhase::Failed
        || phase == WorkbenchOperationPhase::Invalidated;
}
}

std::optional<WorkbenchOperationToken> WorkbenchOperationCoordinator::begin(
    WorkbenchOperationKind operationKind) noexcept
{
    const auto current = state_.load(std::memory_order_acquire);
    if (operationKind == WorkbenchOperationKind::None
        || !terminal(phase(current))
        || currentAuthorityGeneration_.load(std::memory_order_acquire)
            == UINT64_MAX)
    {
        return std::nullopt;
    }
    const auto operationGeneration = nextGeneration_.fetch_add(
        1, std::memory_order_relaxed);
    if (operationGeneration == 0
        || operationGeneration > maximumGeneration)
    {
        return std::nullopt;
    }
    const auto authority = currentAuthorityGeneration_.load(
        std::memory_order_acquire);
    const auto token = WorkbenchOperationToken{
        operationKind,
        operationGeneration,
        authority,
    };
    auto expected = current;
    const auto starting = state(
        token.generation,
        token.kind,
        WorkbenchOperationPhase::Starting);
    if (!state_.compare_exchange_strong(
            expected,
            starting,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return std::nullopt;
    }
    sourceAuthorityGeneration_.store(
        token.authorityGeneration, std::memory_order_relaxed);
    progress_.store(
        progressState(token.generation, 0), std::memory_order_relaxed);
    expected = starting;
    if (currentAuthorityGeneration_.load(std::memory_order_acquire)
            != token.authorityGeneration
        || !state_.compare_exchange_strong(
            expected,
            state(
                token.generation,
                token.kind,
                WorkbenchOperationPhase::Running),
            std::memory_order_release,
            std::memory_order_acquire))
    {
        expected = starting;
        (void) state_.compare_exchange_strong(
            expected,
            state(
                token.generation,
                token.kind,
                WorkbenchOperationPhase::Invalidated),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
        return std::nullopt;
    }
    return token;
}

bool WorkbenchOperationCoordinator::publishProgress(
    WorkbenchOperationToken token,
    double progress) noexcept
{
    if (!token.valid() || !std::isfinite(progress))
        return false;
    const auto currentState = state_.load(std::memory_order_acquire);
    if (generation(currentState) != token.generation
        || kind(currentState) != token.kind
        || phase(currentState) != WorkbenchOperationPhase::Running)
    {
        return false;
    }
    const auto bounded = std::clamp(progress, 0.0, 1.0);
    const auto permille = static_cast<std::uint16_t>(
        std::llround(bounded * 1000.0));
    auto current = progress_.load(std::memory_order_acquire);
    if ((current >> progressGenerationShift) != token.generation)
        return false;
    const auto retained = std::max(
        static_cast<std::uint16_t>(current & 0xffffU), permille);
    const auto desired = progressState(token.generation, retained);
    return desired == current
        || progress_.compare_exchange_strong(
            current,
            desired,
            std::memory_order_release,
            std::memory_order_acquire);
}

bool WorkbenchOperationCoordinator::requestCancel(
    WorkbenchOperationToken token) noexcept
{
    if (!token.valid())
        return false;
    auto current = state_.load(std::memory_order_acquire);
    if (generation(current) != token.generation
        || kind(current) != token.kind)
    {
        return false;
    }
    if (phase(current) == WorkbenchOperationPhase::Cancelling)
        return true;
    if (phase(current) != WorkbenchOperationPhase::Running)
        return false;
    const auto desired = state(
        token.generation, token.kind, WorkbenchOperationPhase::Cancelling);
    return state_.compare_exchange_strong(
        current,
        desired,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool WorkbenchOperationCoordinator::shouldCancel(
    WorkbenchOperationToken token) const noexcept
{
    if (!token.valid())
        return true;
    const auto current = state_.load(std::memory_order_acquire);
    return generation(current) != token.generation
        || kind(current) != token.kind
        || phase(current) != WorkbenchOperationPhase::Running
        || sourceAuthorityGeneration_.load(std::memory_order_relaxed)
            != token.authorityGeneration
        || currentAuthorityGeneration_.load(std::memory_order_acquire)
            != token.authorityGeneration;
}

bool WorkbenchOperationCoordinator::admitPublication(
    WorkbenchOperationToken token) noexcept
{
    if (!token.valid()
        || sourceAuthorityGeneration_.load(std::memory_order_relaxed)
            != token.authorityGeneration
        || currentAuthorityGeneration_.load(std::memory_order_acquire)
            != token.authorityGeneration)
    {
        return false;
    }
    auto current = state_.load(std::memory_order_acquire);
    if (current != state(
            token.generation,
            token.kind,
            WorkbenchOperationPhase::Running))
    {
        return false;
    }
    return state_.compare_exchange_strong(
        current,
        state(
            token.generation,
            token.kind,
            WorkbenchOperationPhase::Publishing),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool WorkbenchOperationCoordinator::invalidatePublication(
    WorkbenchOperationToken token) noexcept
{
    if (!token.valid())
        return false;
    auto expected = state(
        token.generation,
        token.kind,
        WorkbenchOperationPhase::Publishing);
    return state_.compare_exchange_strong(
        expected,
        state(
            token.generation,
            token.kind,
            WorkbenchOperationPhase::Invalidated),
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool WorkbenchOperationCoordinator::finish(
    WorkbenchOperationToken token,
    WorkbenchOperationOutcome outcome) noexcept
{
    if (!token.valid())
        return false;
    auto current = state_.load(std::memory_order_acquire);
    const auto finishing = state(
        token.generation,
        token.kind,
        WorkbenchOperationPhase::Finishing);
    auto terminalPhase = WorkbenchOperationPhase::Failed;
    bool ownsFinishing = false;
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        if (generation(current) != token.generation
            || kind(current) != token.kind)
        {
            return false;
        }
        const auto currentPhase = phase(current);
        if (terminal(currentPhase)
            || currentPhase == WorkbenchOperationPhase::Finishing)
        {
            return false;
        }
        terminalPhase = WorkbenchOperationPhase::Failed;
        if (outcome == WorkbenchOperationOutcome::Cancelled)
            terminalPhase = WorkbenchOperationPhase::Cancelled;
        else if (outcome == WorkbenchOperationOutcome::Succeeded
                 && currentPhase == WorkbenchOperationPhase::Publishing)
        {
            terminalPhase = WorkbenchOperationPhase::Succeeded;
        }
        else if (outcome == WorkbenchOperationOutcome::Succeeded)
        {
            return false;
        }
        if (state_.compare_exchange_strong(
                current,
                finishing,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            ownsFinishing = true;
            break;
        }
    }
    if (!ownsFinishing)
        return false;
    if (terminalPhase == WorkbenchOperationPhase::Succeeded)
    {
        progress_.store(
            progressState(token.generation, 1000),
            std::memory_order_release);
        lastSuccessfulGeneration_.store(
            token.generation, std::memory_order_release);
    }
    state_.store(
        state(token.generation, token.kind, terminalPhase),
        std::memory_order_release);
    return true;
}

void WorkbenchOperationCoordinator::advanceAuthority() noexcept
{
    auto current = state_.load(std::memory_order_acquire);
    for (std::size_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto currentPhase = phase(current);
        if (currentPhase == WorkbenchOperationPhase::Publishing
            || currentPhase == WorkbenchOperationPhase::Finishing
            || terminal(currentPhase))
        {
            break;
        }
        const auto desired = state(
            generation(current),
            kind(current),
            WorkbenchOperationPhase::Invalidated);
        if (state_.compare_exchange_strong(
                current,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            break;
        }
    }
    auto authority = currentAuthorityGeneration_.load(
        std::memory_order_acquire);
    if (authority != UINT64_MAX)
    {
        (void) currentAuthorityGeneration_.compare_exchange_strong(
            authority,
            authority + 1U,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
}

WorkbenchOperationSnapshot WorkbenchOperationCoordinator::snapshot() const noexcept
{
    const auto current = state_.load(std::memory_order_acquire);
    const auto operationGeneration = generation(current);
    const auto progress = progress_.load(std::memory_order_acquire);
    return {
        kind(current),
        phase(current),
        operationGeneration,
        sourceAuthorityGeneration_.load(std::memory_order_relaxed),
        currentAuthorityGeneration_.load(std::memory_order_acquire),
        lastSuccessfulGeneration_.load(std::memory_order_acquire),
        (progress >> progressGenerationShift) == operationGeneration
            ? static_cast<std::uint16_t>(progress & 0xffffU)
            : std::uint16_t{},
    };
}
}
