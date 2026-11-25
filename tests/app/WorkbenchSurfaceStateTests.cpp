#include "talas/app/WorkbenchSurfaceState.h"

#include <array>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testClosedSevenStatePolicy()
{
    using Kind = talas::app::WorkbenchSurfaceStateKind;
    constexpr std::array kinds{
        Kind::Content,
        Kind::Empty,
        Kind::Loading,
        Kind::Unavailable,
        Kind::Stale,
        Kind::Error,
        Kind::Success,
    };
    for (const auto kind : kinds)
    {
        talas::app::WorkbenchSurfaceStateEvidence evidence{
            kind,
            std::string{talas::app::workbenchSurfaceStateName(kind)},
            "Exact state explanation",
            talas::app::WorkbenchSurfaceActionEvidence{
                "surface.retry", "Retry", kind != Kind::Unavailable},
            kind == Kind::Content || kind == Kind::Stale
                || kind == Kind::Error,
        };
        if (kind == Kind::Error)
            evidence.retainsUsableContent = false;
        const auto state = talas::app::workbenchSurfaceState(evidence);
        require(state.has_value(), "every closed surface state should project");
        require(state->kind() == kind
                && !talas::app::workbenchSurfaceStateSymbol(kind).empty()
                && state->action().has_value()
                && state->action()->enabled()
                    == (kind != Kind::Unavailable),
            "projection should retain exact kind, visible symbol, and action truth");
    }
}

void testInvalidOrContradictoryEvidenceFailsClosed()
{
    using Kind = talas::app::WorkbenchSurfaceStateKind;
    auto evidence = talas::app::WorkbenchSurfaceStateEvidence{
        Kind::Loading, "Loading", "Reading the selected project", {}, false};
    require(talas::app::workbenchSurfaceState(evidence).has_value(),
        "loading should be distinct from empty and publish progress truth");

    evidence.kind = Kind::Stale;
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "stale evidence must identify retained content rather than appear live");
    evidence.retainsUsableContent = true;
    require(talas::app::workbenchSurfaceState(evidence).has_value(),
        "stale retained evidence should project explicitly");

    evidence.kind = Kind::Unavailable;
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "unavailable state cannot claim retained usable content");
    evidence.retainsUsableContent = false;
    evidence.title.clear();
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "missing user-facing state evidence must fail closed");

    evidence.title = "Unavailable";
    evidence.kind = Kind::Content;
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "content must identify currently usable content rather than a blank region");

    evidence.retainsUsableContent = true;
    evidence.title.assign(161, 'x');
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "overlong presentation evidence must fail before publication");

    evidence.title = "Unknown";
    evidence.kind = static_cast<Kind>(99);
    require(!talas::app::workbenchSurfaceState(evidence).has_value(),
        "unknown state values must never inherit plausible presentation");
}

void testAnnouncementsAreSemanticAndDeduplicated()
{
    using Kind = talas::app::WorkbenchSurfaceStateKind;
    const auto empty = talas::app::workbenchSurfaceState({
        Kind::Empty, "No diagnostics", "No issue is currently reported", {}, false});
    require(empty.has_value()
            && talas::app::shouldAnnounceWorkbenchSurfaceState({}, *empty)
            && !talas::app::shouldAnnounceWorkbenchSurfaceState(empty, *empty),
        "live announcements should publish once per semantic state transition");

    const auto loading = talas::app::workbenchSurfaceState({
        Kind::Loading,
        "Asset recovery in progress",
        "421 files scanned",
        talas::app::WorkbenchSurfaceActionEvidence{
            "asset-recovery.cancel", "Cancel", true},
        false});
    const auto loadingProgress = talas::app::workbenchSurfaceState({
        Kind::Loading,
        "Asset recovery in progress",
        "422 files scanned",
        talas::app::WorkbenchSurfaceActionEvidence{
            "asset-recovery.cancel", "Cancel", true},
        false});
    require(loading.has_value() && loadingProgress.has_value()
            && !talas::app::shouldAnnounceWorkbenchSurfaceState(
                loading, *loadingProgress),
        "progress detail changes must refresh visually without repeating the same live announcement");

    const auto content = talas::app::workbenchSurfaceState({
        Kind::Content, "Diagnostics", "One issue is available", {}, true});
    require(content.has_value()
            && !talas::app::shouldAnnounceWorkbenchSurfaceState(empty, *content),
        "ordinary content refreshes should not create live-region churn");
}

void testRepresentativeWorkbenchSurfacesUseExactActionTruth()
{
    using Kind = talas::app::WorkbenchSurfaceStateKind;
    struct Fixture
    {
        const char* surface;
        Kind kind;
        const char* actionId;
        bool actionEnabled;
        bool retained;
    };
    constexpr std::array fixtures{
        Fixture{"Patch", Kind::Content, "graph.add-node", true, true},
        Fixture{"Automation", Kind::Empty, "timeline.add-event", true, false},
        Fixture{"Analysis", Kind::Stale, "analysis.refresh", true, true},
        Fixture{"Performance", Kind::Unavailable, "performance.prepare", false, false},
        Fixture{"Samples", Kind::Loading, "samples.cancel-import", true, false},
        Fixture{"Library", Kind::Empty, "library.clear-search", true, false},
        Fixture{"Inspector", Kind::Content, "inspector.edit", true, true},
        Fixture{"Diagnostics", Kind::Error, "diagnostics.open-target", true, true},
        Fixture{"Recovery", Kind::Success, "recovery.close", true, false},
        Fixture{"Component replacement", Kind::Loading, "replacement.cancel", true, false},
    };
    for (const auto& fixture : fixtures)
    {
        const auto state = talas::app::workbenchSurfaceState({
            fixture.kind,
            fixture.surface,
            "Representative exact state explanation",
            talas::app::WorkbenchSurfaceActionEvidence{
                fixture.actionId, "Exact action", fixture.actionEnabled},
            fixture.retained,
        });
        require(state.has_value()
                && state->action().has_value()
                && state->action()->id() == fixture.actionId
                && state->action()->enabled() == fixture.actionEnabled,
            std::string{fixture.surface}
                + " did not preserve its exact action identity or enablement");
    }
}
}

int main()
{
    testClosedSevenStatePolicy();
    testInvalidOrContradictoryEvidenceFailsClosed();
    testAnnouncementsAreSemanticAndDeduplicated();
    testRepresentativeWorkbenchSurfacesUseExactActionTruth();
    return 0;
}
