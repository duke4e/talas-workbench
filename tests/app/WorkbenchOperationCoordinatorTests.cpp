#include "talas/app/WorkbenchOperationCoordinator.h"

#include <atomic>
#include <stdexcept>
#include <thread>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testGenerationProgressCancellationAndLastGood()
{
    talas::app::WorkbenchOperationCoordinator coordinator;
    const auto first = coordinator.begin(
        talas::app::WorkbenchOperationKind::AudioExport);
    require(first.has_value(), "first operation should begin");
    require(coordinator.publishProgress(*first, 0.6),
            "current operation should accept progress");
    require(coordinator.publishProgress(*first, 0.2),
            "regressed progress should be ignored without invalidating the operation");
    require(coordinator.snapshot().progressPermille == 600,
            "progress must remain monotonic");
    require(coordinator.admitPublication(*first),
            "current authority should admit terminal publication");
    require(coordinator.finish(
                *first, talas::app::WorkbenchOperationOutcome::Succeeded),
            "published operation should finish successfully");
    require(coordinator.snapshot().lastSuccessfulGeneration
                == first->generation,
            "successful generation must remain available as last-good truth");
    require(!coordinator.finish(
                *first, talas::app::WorkbenchOperationOutcome::Failed)
                && coordinator.snapshot().phase
                    == talas::app::WorkbenchOperationPhase::Succeeded,
            "terminal completion must be exactly once and immutable");

    const auto publicationInvalidation = coordinator.begin(
        talas::app::WorkbenchOperationKind::AudioExport);
    require(publicationInvalidation.has_value()
            && coordinator.admitPublication(*publicationInvalidation)
            && coordinator.invalidatePublication(*publicationInvalidation)
            && coordinator.snapshot().phase
                == talas::app::WorkbenchOperationPhase::Invalidated,
        "authority loss after publication admission must retain Invalidated terminal truth");

    const auto second = coordinator.begin(
        talas::app::WorkbenchOperationKind::ProjectPackage);
    require(second.has_value(), "second operation should begin");
    coordinator.advanceAuthority();
    require(coordinator.shouldCancel(*second),
            "authority advance must cancel the captured generation");
    require(!coordinator.admitPublication(*second),
            "invalidated generation must not publish");
    require(!coordinator.finish(
                *second, talas::app::WorkbenchOperationOutcome::Cancelled),
            "authority invalidation already owns the terminal transition");
    const auto invalidated = coordinator.snapshot();
    require(invalidated.phase
                == talas::app::WorkbenchOperationPhase::Invalidated
            && invalidated.lastSuccessfulGeneration == first->generation,
            "invalidation must retain prior last-good generation");
}

void testPublicationAndInvalidationHaveOneLinearizationWinner()
{
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        talas::app::WorkbenchOperationCoordinator coordinator;
        const auto token = coordinator.begin(
            talas::app::WorkbenchOperationKind::AudioExport);
        require(token.has_value(), "race operation should begin");
        std::atomic<bool> start = false;
        bool admitted = false;
        std::thread publisher([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            admitted = coordinator.admitPublication(*token);
        });
        std::thread invalidator([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            coordinator.advanceAuthority();
        });
        start.store(true, std::memory_order_release);
        publisher.join();
        invalidator.join();
        const auto snapshot = coordinator.snapshot();
        require((admitted
                    && snapshot.phase
                        == talas::app::WorkbenchOperationPhase::Publishing)
                || (!admitted
                    && snapshot.phase
                        == talas::app::WorkbenchOperationPhase::Invalidated),
            "publication and invalidation must have exactly one winner");
    }
}

void testConcurrentBeginHasOneAdmissionWinner()
{
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        talas::app::WorkbenchOperationCoordinator coordinator;
        std::atomic<bool> start = false;
        std::optional<talas::app::WorkbenchOperationToken> first;
        std::optional<talas::app::WorkbenchOperationToken> second;
        std::thread one([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            first = coordinator.begin(
                talas::app::WorkbenchOperationKind::AudioExport);
        });
        std::thread two([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            second = coordinator.begin(
                talas::app::WorkbenchOperationKind::ProjectPackage);
        });
        start.store(true, std::memory_order_release);
        one.join();
        two.join();
        require(first.has_value() != second.has_value(),
                "concurrent begin calls must have exactly one winner");
        const auto winner = first.has_value() ? *first : *second;
        const auto snapshot = coordinator.snapshot();
        require(snapshot.phase
                    == talas::app::WorkbenchOperationPhase::Running
                    && snapshot.generation == winner.generation
                    && snapshot.kind == winner.kind,
                "the admitted begin token must own the published running state");
    }
}

void testConcurrentCancelAndFinishCannotStrandCancelling()
{
    for (int iteration = 0; iteration < 2000; ++iteration)
    {
        talas::app::WorkbenchOperationCoordinator coordinator;
        const auto token = coordinator.begin(
            talas::app::WorkbenchOperationKind::AudioExport);
        require(token.has_value(), "cancel race operation should begin");
        std::atomic<bool> start = false;
        std::thread canceller([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void) coordinator.requestCancel(*token);
        });
        std::thread finisher([&] {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            (void) coordinator.finish(
                *token, talas::app::WorkbenchOperationOutcome::Cancelled);
        });
        start.store(true, std::memory_order_release);
        canceller.join();
        finisher.join();
        require(
            coordinator.snapshot().phase
                == talas::app::WorkbenchOperationPhase::Cancelled,
            "cancel/finish race must reach one terminal Cancelled state");
        require(
            coordinator.begin(talas::app::WorkbenchOperationKind::ProjectPackage)
                .has_value(),
            "cancel/finish race must not strand future operations");
    }
}
}

int main()
{
    testGenerationProgressCancellationAndLastGood();
    testPublicationAndInvalidationHaveOneLinearizationWinner();
    testConcurrentBeginHasOneAdmissionWinner();
    testConcurrentCancelAndFinishCannotStrandCancelling();
}
