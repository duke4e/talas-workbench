#include "talas/app/SnapshotWorkbenchModel.h"
#include "talas/core/BuiltinNodes.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string source)
{
    talas::project::ProjectParameterExpression value;
    value.parameterId = std::move(id);
    value.expression = std::move(source);
    return value;
}

talas::project::SnapshotTargetIdentity gainTarget()
{
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    target.nodeId = "vca";
    target.parameterId = "gain";
    return target;
}

talas::project::ProjectDocument snapshotDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"source", "talas.noise", {}},
        {"vca", "talas.vca", {expression("gain", "0.25")}},
        {"output", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"source", "audioOut", "vca", "audioIn"},
        {"vca", "audioOut", "output", "audioIn"},
    };
    talas::project::SnapshotValue quiet;
    quiet.number = 0.0;
    talas::project::SnapshotValue loud;
    loud.number = 1.0;
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "gainBank";
    bank.displayName = "Gain Bank";
    bank.scope.kind = talas::project::SnapshotScopeKind::Node;
    bank.scope.nodeId = "vca";
    bank.smoothingSeconds = 0.125;
    bank.parameterPolicies.push_back({
        gainTarget(),
        true,
        talas::project::SnapshotParameterRange{0.0, 1.0},
    });
    bank.snapshots = {
        {"quiet", "Quiet", {{gainTarget(), quiet}}},
        {"loud", "Loud", {{gainTarget(), loud}}},
    };
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

talas::project::ProjectDocument scopedCaptureDocument()
{
    auto document = snapshotDocument();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice-def";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back(
        {"inner-vca", "talas.vca", {expression("gain", "0.5")}});
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice";
    instance.soundDefinitionId = "voice-def";
    document.graph.nodes.push_back(std::move(instance));
    return document;
}

void testCaptureScopeOptionsExposeExactProjectNodeAndDefinitionIdentities()
{
    const auto document = scopedCaptureDocument();
    const auto soundScopes = talas::app::snapshotWorkbenchCaptureScopes(
        document,
        "voice");
    require(
        soundScopes.size() == 3
            && soundScopes[0].scope.kind
                == talas::project::SnapshotScopeKind::Node
            && soundScopes[0].scope.nodeId == "voice"
            && soundScopes[0].scope.instancePath.empty()
            && soundScopes[1].scope.kind
                == talas::project::SnapshotScopeKind::Definition
            && soundScopes[1].scope.instancePath
                == std::vector<std::string>{"voice"}
            && soundScopes[1].scope.definitionId == "voice-def"
            && soundScopes[2].scope
                == talas::project::SnapshotScopeIdentity{}
            && soundScopes[0].displayName.find("Selected Node")
                != std::string::npos
            && soundScopes[1].accessibleDescription.find(
                   "voice-def") != std::string::npos,
        "a selected reusable Sound should offer exact node, definition, and project capture scopes");

    const auto primitiveScopes = talas::app::snapshotWorkbenchCaptureScopes(
        document,
        "vca");
    require(
        primitiveScopes.size() == 2
            && primitiveScopes[0].scope.kind
                == talas::project::SnapshotScopeKind::Node
            && primitiveScopes[0].scope.nodeId == "vca"
            && primitiveScopes[1].scope.kind
                == talas::project::SnapshotScopeKind::Project,
        "a selected primitive node should offer node and project capture scopes only");

    const auto staleScopes = talas::app::snapshotWorkbenchCaptureScopes(
        document,
        "missing");
    require(
        staleScopes.size() == 1
            && staleScopes.front().scope.kind
                == talas::project::SnapshotScopeKind::Project,
        "a stale selection should fall back to a valid project capture scope");
}

void testNestedCaptureContextUsesAnExactConcreteInstancePath()
{
    auto document = scopedCaptureDocument();
    auto secondVoice = document.graph.nodes.back();
    secondVoice.instanceId = "voice-b";
    document.graph.nodes.push_back(std::move(secondVoice));

    const auto preferred = talas::app::snapshotWorkbenchCaptureContext(
        document,
        "inner-vca",
        std::optional<std::string_view>{"voice-def"},
        "voice-b");
    const auto scopes = talas::app::snapshotWorkbenchCaptureScopes(
        document,
        preferred);
    require(
        preferred.exact
            && preferred.instancePath
                == std::vector<std::string>{"voice-b"}
            && preferred.containingDefinitionId
                == std::optional<std::string>{"voice-def"}
            && scopes.size() == 3
            && scopes[0].scope.kind
                == talas::project::SnapshotScopeKind::Node
            && scopes[0].scope.instancePath
                == std::vector<std::string>{"voice-b"}
            && scopes[0].scope.containingDefinitionId
                == std::optional<std::string>{"voice-def"}
            && scopes[0].scope.nodeId == "inner-vca"
            && scopes[1].scope.kind
                == talas::project::SnapshotScopeKind::Definition
            && scopes[1].scope.instancePath
                == std::vector<std::string>{"voice-b"}
            && scopes[1].scope.definitionId == "voice-def",
        "an active nested graph should expose exact selected-node and containing-definition scopes through the preferred root instance");

    const auto ambiguous = talas::app::snapshotWorkbenchCaptureContext(
        document,
        "inner-vca",
        std::optional<std::string_view>{"voice-def"},
        {});
    const auto ambiguousScopes =
        talas::app::snapshotWorkbenchCaptureScopes(document, ambiguous);
    require(
        !ambiguous.exact
            && ambiguousScopes.size() == 1
            && ambiguousScopes.front().scope.kind
                == talas::project::SnapshotScopeKind::Project,
        "an ambiguous nested definition must not guess one concrete instance path");
}

void testEmptyProjectionNamesTheCaptureScopeAndAction()
{
    auto document = snapshotDocument();
    document.snapshotLibrary.banks.clear();
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        {},
        {});
    require(projection.banks.empty()
            && projection.captureScopeBreadcrumb == "Node / vca"
            && projection.emptyTitle == "No snapshot banks"
            && projection.emptyDescription.find("Node / vca")
                != std::string::npos
            && !projection.canRecall,
        "empty state should explain the exact next capture scope without enabling live actions");
}

void testTypicalProjectionCarriesActiveMorphLockAndTransitionState()
{
    const auto document = snapshotDocument();
    talas::runtime::SnapshotPerformanceState state;
    state.currentValues.push_back({gainTarget(), {}});
    state.currentValues.front().value.number = 0.75;
    state.activeBankId = "gainBank";
    state.activeSnapshotId = "loud";
    state.morphLeftSnapshotId = "quiet";
    state.morphRightSnapshotId = "loud";
    state.morphPosition = 0.75;
    state.lastRandomSeed = 0x1234;
    state.morphActive = true;
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        state,
        "vca",
        "gainBank",
        "loud",
        {},
        true,
        true);
    require(projection.banks.size() == 1
            && projection.snapshots.size() == 2
            && projection.snapshots[0].morphLeft
            && projection.snapshots[1].active
            && projection.snapshots[1].morphRight
            && projection.parameters.size() == 1
            && projection.parameters.front().locked
            && projection.parameters.front().range == "0 ... 1"
            && projection.parameters.front().currentValue == "0.75"
            && projection.transitionActive
            && projection.transitionCoalesced
            && projection.status.find("newest") != std::string::npos
            && projection.canRecall
            && projection.canMorph
            && projection.canRandomize,
        "typical projection should expose textual active/A/B/lock/range and bounded transition cues");
}

void testStaleAndMissingRangeStatesDisableOnlyUnsafeActions()
{
    auto document = snapshotDocument();
    document.snapshotLibrary.banks.front()
        .parameterPolicies.front().range.reset();
    document.snapshotLibrary.banks.front()
        .parameterPolicies.front().locked = false;
    talas::project::SnapshotDiagnostic stale;
    stale.code = talas::project::SnapshotDiagnosticCode::MissingParameter;
    stale.severity = talas::project::SnapshotDiagnosticSeverity::Error;
    stale.targetIdentity =
        talas::project::snapshotTargetIdentityString(gainTarget());
    stale.message = "Gain target is stale";
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        "gainBank",
        "quiet",
        std::span<const talas::project::SnapshotDiagnostic>{&stale, 1});
    require(!projection.canRecall
            && !projection.canRandomize
            && !projection.canRandomMerge
            && projection.canStore
            && projection.snapshots.front().diagnostic
                == "Gain target is stale"
            && projection.parameters.front().diagnostic.find("range")
                != std::string::npos
            && projection.parameters.front().accessibleDescription.find(
                   "locked") != std::string::npos,
        "stale targets and missing ranges should stay visible, explain recovery, and disable only unsafe actions");
}

void testLockedTargetsNeedNoRandomRange()
{
    auto document = snapshotDocument();
    document.snapshotLibrary.banks.front()
        .parameterPolicies.front().range.reset();
    const auto locked = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        "gainBank",
        "quiet");
    require(locked.canRandomize
            && locked.parameters.front().rangeAvailable
            && locked.parameters.front().diagnostic.empty(),
        "a locked target is not randomized, so its missing range must not disable Randomize");

    document.snapshotLibrary.banks.front()
        .parameterPolicies.front().locked = false;
    const auto unlocked = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        "gainBank",
        "quiet");
    require(!unlocked.canRandomize
            && !unlocked.parameters.front().rangeAvailable,
        "an unlocked numeric target still needs a range");
}

void testOneIncompatibleSnapshotDoesNotDisableAValidMorphPair()
{
    auto document = snapshotDocument();
    document.snapshotLibrary.banks.front().snapshots.push_back({
        "stale",
        "Stale",
        {},
    });
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        "gainBank",
        "quiet");
    require(
        projection.snapshots.size() == 3
            && projection.snapshots[0].recallEnabled
            && projection.snapshots[1].recallEnabled
            && !projection.snapshots[2].recallEnabled
            && projection.canMorph,
        "one incompatible snapshot must not disable morph between two complete endpoints");
}

void testStoreIsUnavailableWithoutACurrentAcceptedPerformanceRevision()
{
    const auto document = snapshotDocument();
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        "vca",
        "gainBank",
        "quiet",
        {},
        false,
        false,
        false);
    require(
        !projection.canStore
            && projection.status.find("Stage") != std::string::npos,
        "Store must stay disabled until the accepted performance revision matches authoring bytes");
}

void testMaximumProjectionPreservesAllOrderedRows()
{
    auto document = snapshotDocument();
    auto& bank = document.snapshotLibrary.banks.front();
    const auto prototype = bank.snapshots.front();
    bank.snapshots.clear();
    for (std::size_t index = 0;
         index < talas::project::maximumSnapshotsPerBank;
         ++index)
    {
        auto snapshot = prototype;
        snapshot.snapshotId = "snapshot" + std::to_string(index);
        snapshot.displayName = "Snapshot " + std::to_string(index);
        bank.snapshots.push_back(std::move(snapshot));
    }
    const auto projection = talas::app::snapshotWorkbenchProjection(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        {},
        {},
        "gainBank",
        "snapshot127");
    require(projection.snapshots.size()
                == talas::project::maximumSnapshotsPerBank
            && projection.snapshots.front().snapshotId == "snapshot0"
            && projection.snapshots.back().snapshotId == "snapshot127",
        "maximum bounded banks should retain canonical row order without truncating the projection");
}
}

int main()
{
    testCaptureScopeOptionsExposeExactProjectNodeAndDefinitionIdentities();
    testNestedCaptureContextUsesAnExactConcreteInstancePath();
    testEmptyProjectionNamesTheCaptureScopeAndAction();
    testTypicalProjectionCarriesActiveMorphLockAndTransitionState();
    testStaleAndMissingRangeStatesDisableOnlyUnsafeActions();
    testLockedTargetsNeedNoRandomRange();
    testOneIncompatibleSnapshotDoesNotDisableAValidMorphPair();
    testStoreIsUnavailableWithoutACurrentAcceptedPerformanceRevision();
    testMaximumProjectionPreservesAllOrderedRows();
    return 0;
}
