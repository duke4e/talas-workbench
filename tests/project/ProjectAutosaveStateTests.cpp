#include "talas/project/ProjectAutosaveState.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testDirtyChangeDebouncesAndCleanStateClears()
{
    talas::project::ProjectAutosaveState state(2000.0, 5000.0, 30000.0);
    state.documentChanged(true, 100.0);
    require(state.pending(), "dirty change should schedule autosave");
    require(!state.shouldStartWrite(true, false, 2099.0),
            "autosave should wait for the full quiet period");
    require(state.shouldStartWrite(true, false, 2100.0),
            "autosave should become ready at the debounce boundary");
    require(!state.shouldStartWrite(true, true, 2100.0),
            "autosave should not start while a writer is active");

    state.documentChanged(false, 2200.0);
    require(!state.pending(), "clean document should clear pending autosave");
    require(!state.hasWriteFailure(),
            "clean document should clear the persistent failure indicator");
}

void testFailureRetriesWithoutAnotherEditAndBackoffIsBounded()
{
    talas::project::ProjectAutosaveState state(2000.0, 5000.0, 12000.0);
    state.documentChanged(true, 0.0);
    state.writeStarted();
    state.writeFinished(false, true, 2100.0);
    require(state.pending(), "failed autosave should reschedule itself");
    require(state.hasWriteFailure(),
            "failed autosave should retain a persistent failure indicator");
    require(!state.shouldStartWrite(true, false, 7099.0),
            "first retry should honor its backoff");
    require(state.shouldStartWrite(true, false, 7100.0),
            "first retry should run without an intervening edit");

    state.writeStarted();
    state.writeFinished(false, true, 7200.0);
    require(!state.shouldStartWrite(true, false, 17199.0),
            "second failure should increase retry backoff");
    require(state.shouldStartWrite(true, false, 17200.0),
            "second retry should start at the increased boundary");

    state.writeStarted();
    state.writeFinished(false, true, 17300.0);
    require(!state.shouldStartWrite(true, false, 29299.0),
            "retry delay should respect its configured cap");
    require(state.shouldStartWrite(true, false, 29300.0),
            "capped retry should remain finite");

    state.writeStarted();
    state.writeFinished(true, true, 29400.0);
    require(!state.pending(), "successful retry should clear its pending state");
    require(!state.hasWriteFailure(),
            "successful retry should clear the failure indicator");
}

void testEditDuringWriteKeepsLatestSnapshotPending()
{
    talas::project::ProjectAutosaveState state(2000.0, 5000.0, 30000.0);
    state.documentChanged(true, 0.0);
    state.writeStarted();
    state.documentChanged(true, 2500.0);
    state.writeFinished(false, true, 2600.0);

    require(!state.shouldStartWrite(true, false, 4499.0),
            "failure of an older snapshot should not bypass the newer edit debounce");
    require(state.shouldStartWrite(true, false, 4500.0),
            "newer edit should retain its original pending deadline");
}
}

int main()
{
    testDirtyChangeDebouncesAndCleanStateClears();
    testFailureRetriesWithoutAnotherEditAndBackoffIsBounded();
    testEditDuringWriteKeepsLatestSnapshotPending();
    return 0;
}
