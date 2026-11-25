#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSnapshots.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::core::NodeRegistry makeRegistry()
{
    talas::core::NodeRegistry registry;
    talas::core::registerBuiltinNodeTypes(registry);
    return registry;
}

talas::project::SnapshotValueEntry numericNodeValue(
    std::string nodeId,
    std::string parameterId,
    double value)
{
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    target.nodeId = std::move(nodeId);
    target.parameterId = std::move(parameterId);
    talas::project::SnapshotValue snapshotValue;
    snapshotValue.number = value;
    return {std::move(target), std::move(snapshotValue)};
}

talas::project::SnapshotTargetIdentity eventTarget(std::string name)
{
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::EventValue;
    target.eventName = std::move(name);
    target.domain = talas::expr::ValueDomain::Scalar;
    return target;
}

talas::project::SnapshotValueEntry numericValue(
    talas::project::SnapshotTargetIdentity target,
    double value)
{
    talas::project::SnapshotValue snapshotValue;
    snapshotValue.number = value;
    return {std::move(target), std::move(snapshotValue)};
}

void testMissingParameterRemainsPortableAndProducesDiagnostic()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"osc-main", "talas.oscillator", {}});
    talas::project::reconcileProjectPresentation(document);

    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "stale";
    snapshot.displayName = "Stale";
    snapshot.values.push_back(
        numericNodeValue("osc-main", "removedFrequency", 440.0));

    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "project-bank";
    bank.displayName = "Project Bank";
    bank.snapshots.push_back(snapshot);
    document.snapshotLibrary.banks.push_back(bank);

    const auto canonical =
        talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(canonical);
    require(
        loaded.ok(),
        "well-formed stale snapshot targets should remain loadable");
    require(
        loaded.document.snapshotLibrary == document.snapshotLibrary,
        "loading should preserve the stale target without rebinding");

    const auto diagnostics = talas::project::snapshotLibraryDiagnostics(
        loaded.document,
        makeRegistry());
    require(
        diagnostics.size() == 1
            && diagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::MissingParameter
            && diagnostics.front().path
                == "$.snapshotLibrary.banks[0].snapshots[0].values[0].target",
        "stale node parameters should produce one exact recoverable diagnostic");
    require(
        talas::project::serializeProjectDocument(loaded.document) == canonical,
        "diagnostic discovery must not mutate canonical stale target bytes");
}

void testSoundParameterAliasIsReportedButNeverAppliedSilently()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundExposedParameter exposed;
    exposed.parameterId = "gain";
    exposed.displayName = "Gain";
    exposed.defaultValue.parameterId = "gain";
    exposed.defaultValue.expression = "1";
    exposed.aliases = {"oldGain"};
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice-def";
    definition.displayName = "Voice";
    definition.exposedParameters.push_back(exposed);
    document.soundDefinitions.push_back(definition);
    talas::project::ProjectNode voice;
    voice.instanceId = "voice-a";
    voice.soundDefinitionId = "voice-def";
    document.graph.nodes.push_back(voice);
    talas::project::reconcileProjectPresentation(document);

    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::SoundParameter;
    target.instancePath = {"voice-a"};
    target.definitionId = "voice-def";
    target.parameterId = "oldGain";
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "legacy";
    snapshot.displayName = "Legacy";
    snapshot.values.push_back({target, {}});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "project-bank";
    bank.displayName = "Project Bank";
    bank.snapshots.push_back(snapshot);
    document.snapshotLibrary.banks.push_back(bank);

    const auto diagnostics = talas::project::snapshotLibraryDiagnostics(
        document,
        makeRegistry());
    require(
        diagnostics.size() == 1
            && diagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::AliasAvailable,
        "a Sound alias should be offered as an explicit migration diagnostic");
    require(
        document.snapshotLibrary.banks.front()
                .snapshots.front()
                .values.front()
                .target.parameterId
            == "oldGain",
        "diagnostic discovery must never apply a compatibility alias");
}

void testMorphUsesCanonicalTargetsLocksAndExactDiscreteRule()
{
    const auto amount = eventTarget("Amount");
    const auto enabled = eventTarget("Enabled");
    const auto mode = eventTarget("Mode");

    talas::project::SnapshotValue falseValue;
    falseValue.kind = talas::project::SnapshotValueKind::Boolean;
    talas::project::SnapshotValue trueValue = falseValue;
    trueValue.boolean = true;
    talas::project::SnapshotValue modeA;
    modeA.kind = talas::project::SnapshotValueKind::Choice;
    modeA.choice = "A";
    auto modeB = modeA;
    modeB.choice = "B";

    talas::project::ProjectSnapshot left;
    left.values = {
        {mode, modeA},
        numericValue(amount, 0.0),
        {enabled, falseValue},
    };
    talas::project::ProjectSnapshot right;
    right.values = {
        {enabled, trueValue},
        numericValue(amount, 1.0),
        {mode, modeB},
    };
    const std::vector<talas::project::SnapshotValueEntry> current{
        numericValue(amount, 0.25),
        {enabled, falseValue},
        {mode, modeA},
    };
    const std::vector<talas::project::SnapshotParameterPolicy> policies{
        {amount, true, std::nullopt},
    };

    const auto below = talas::project::morphSnapshotValues(
        left,
        right,
        current,
        policies,
        0.499);
    const auto midpoint = talas::project::morphSnapshotValues(
        left,
        right,
        current,
        policies,
        0.5);
    require(below.ok() && midpoint.ok(),
            "compatible snapshot morphs should succeed");
    require(
        midpoint.values.size() == 3
            && std::ranges::is_sorted(
                midpoint.values,
                {},
                [](const auto& entry) {
                    return talas::project::snapshotTargetIdentityString(
                        entry.target);
                }),
        "morph output should use canonical target order");
    const auto find = [](const auto& values, const auto& target)
        -> const talas::project::SnapshotValue& {
        const auto found = std::ranges::find_if(
            values,
            [&](const auto& entry) {
                return entry.target == target;
            });
        require(found != values.end(), "morph should retain every target");
        return found->value;
    };
    require(
        find(midpoint.values, amount).number == 0.25,
        "locked morph targets should hold their gesture-start value");
    require(
        !find(below.values, enabled).boolean
            && find(midpoint.values, enabled).boolean,
        "Boolean morph should select B exactly at one half");
    require(
        find(below.values, mode).choice == "A"
            && find(midpoint.values, mode).choice == "B",
        "Choice morph should select B exactly at one half");
}

void testSeededRandomizationIsExactAndLockStable()
{
    const auto first = eventTarget("A");
    const auto second = eventTarget("B");
    const std::vector<talas::project::SnapshotRandomizationTarget> targets{
        {numericValue(second, 0.0), false,
         talas::project::SnapshotParameterRange{0.0, 1.0}, {}},
        {numericValue(first, 10.0), false,
         talas::project::SnapshotParameterRange{10.0, 20.0}, {}},
    };
    const auto randomized =
        talas::project::randomizeSnapshotValues(targets, 0);
    require(randomized.ok() && randomized.values.size() == 2,
            "seeded numeric randomization should succeed");
    require(
        randomized.values.front().target == first
            && std::abs(
                   randomized.values.front().value.number
                       - 18.833108082136426)
                < 1.0e-12,
        "seed zero should reproduce the frozen SplitMix64 first draw");

    auto lockedTargets = targets;
    lockedTargets[1].locked = true;
    const auto locked =
        talas::project::randomizeSnapshotValues(lockedTargets, 0);
    require(locked.ok(), "locked randomization should remain valid");
    require(
        locked.values.front().value.number == 10.0
            && locked.values.back().value.number
                == randomized.values.back().value.number,
        "locked targets should consume a draw without shifting later targets");

    auto missingRange = targets;
    missingRange.front().range.reset();
    const auto rejected =
        talas::project::randomizeSnapshotValues(missingRange, 0);
    require(
        !rejected.ok() && rejected.values.empty()
            && std::ranges::any_of(
                rejected.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                        == talas::project::SnapshotDiagnosticCode::MissingRandomRange;
                }),
        "a missing numeric range should reject the complete random action");

    auto lockedWithoutRange = missingRange;
    lockedWithoutRange.front().locked = true;
    const auto lockedOnly =
        talas::project::randomizeSnapshotValues(lockedWithoutRange, 0);
    require(lockedOnly.ok()
            && lockedOnly.values.back().value.number
                == missingRange.front().current.value.number
            && talas::project::randomMergeSnapshotValues(
                lockedWithoutRange, 0, 0.5).ok(),
        "a locked target keeps its value and needs no random range");

    const std::vector<talas::project::SnapshotRandomizationTarget> widest{
        {numericValue(eventTarget("Wide"), 0.0), false,
         talas::project::SnapshotParameterRange{
             -std::numeric_limits<double>::max(),
             std::numeric_limits<double>::max()},
         {}},
    };
    for (std::uint64_t seed = 0; seed < 8; ++seed)
    {
        const auto wide = talas::project::randomizeSnapshotValues(widest, seed);
        require(wide.ok()
                && std::isfinite(wide.values.front().value.number),
            "a range as wide as the double range must randomize finitely");
    }

    const auto merged =
        talas::project::randomMergeSnapshotValues(targets, 0, 0.25);
    require(
        merged.ok()
            && std::abs(
                   merged.values.front().value.number
                       - (10.0
                          + (18.833108082136426 - 10.0) * 0.25))
                < 1.0e-12,
        "random merge should interpolate current to the deterministic draw");

    const std::vector<talas::project::SnapshotRandomizationTarget>
        oppositeSignRange{
            {numericValue(eventTarget("Opposite"), 0.0), false,
             talas::project::SnapshotParameterRange{-10.0, 20.0}, {}},
        };
    const auto exactFormula = talas::project::randomizeSnapshotValues(
        oppositeSignRange,
        0);
    require(
        exactFormula.ok()
            && exactFormula.values.front().value.number
                == 0x1.07fd3b6bc6d7dp+4,
        "numeric randomization must use the frozen minimum + unit * (maximum - minimum) evaluation order");
}

void testOperationDiagnosticsAreBoundedWithOneTruncationMarker()
{
    std::vector<talas::project::SnapshotRandomizationTarget> randomTargets;
    std::vector<talas::project::SnapshotScopeTarget> scopeTargets;
    talas::project::ProjectSnapshot left;
    talas::project::ProjectSnapshot right;
    randomTargets.reserve(talas::project::maximumSnapshotValuesPerSnapshot);
    scopeTargets.reserve(talas::project::maximumSnapshotValuesPerSnapshot);
    left.values.reserve(talas::project::maximumSnapshotValuesPerSnapshot);
    right.values.reserve(talas::project::maximumSnapshotValuesPerSnapshot);
    for (std::size_t index = 0;
         index < talas::project::maximumSnapshotValuesPerSnapshot;
         ++index)
    {
        auto target = eventTarget("Target" + std::to_string(index));
        randomTargets.push_back({
            numericValue(target, 0.0),
            false,
            std::nullopt,
            {},
        });
        talas::project::SnapshotScopeTarget scopeTarget;
        scopeTarget.target = target;
        scopeTargets.push_back(std::move(scopeTarget));
        left.values.push_back(numericValue(target, 0.0));
        right.values.push_back(numericValue(
            eventTarget("Other" + std::to_string(index)),
            1.0));
    }
    const auto requireBounded = [](const auto& result,
                                   std::string_view operation) {
        require(
            result.diagnostics.size()
                    == talas::project::maximumSnapshotDiagnosticCount + 1
                && result.diagnostics.back().code
                    == talas::project::SnapshotDiagnosticCode::CapacityExceeded
                && result.diagnostics.back().message.find("truncated")
                    != std::string::npos,
            std::string(operation)
                + " diagnostics must retain 256 entries plus one truncation marker");
    };
    requireBounded(
        talas::project::randomizeSnapshotValues(randomTargets, 0),
        "randomization");
    requireBounded(
        talas::project::initializeSnapshotScopeValues(scopeTargets),
        "initialization");
    requireBounded(
        talas::project::captureSnapshotScopeValues(scopeTargets, {}),
        "capture");
    requireBounded(
        talas::project::morphSnapshotValues(left, right, {}, {}, 0.5),
        "morph");
}

talas::project::ProjectDocument makeScopedSnapshotDocument(
    talas::core::NodeRegistry& registry)
{
    talas::core::NodeTypeDescriptor fixture;
    fixture.typeId = "talas.snapshotFixture";
    fixture.displayName = "Snapshot Fixture";
    fixture.category = "Tests";
    fixture.parameters = {
        {"gain", "series(0.1, 0.15, 3)[1]", talas::expr::ValueDomain::Scalar},
        talas::core::choiceParameter("mode", "slow", {"slow", "fast"}),
        talas::core::booleanParameter("enabled", true),
        talas::core::resourceParameter("sample", "fixture.wav"),
        talas::core::textParameter("label", "Fixture"),
    };
    registry.addNodeType(std::move(fixture));

    talas::project::ProjectSoundExposedParameter childDepth;
    childDepth.parameterId = "depth";
    childDepth.displayName = "Depth";
    childDepth.defaultValue.parameterId = "depth";
    childDepth.defaultValue.expression = "0.5";

    talas::project::ProjectSoundDefinition childDefinition;
    childDefinition.definitionId = "child-def";
    childDefinition.displayName = "Child";
    childDefinition.exposedParameters.push_back(childDepth);
    childDefinition.graph.nodes.push_back(
        {"hidden-child-node", "talas.snapshotFixture", {}});

    talas::project::ProjectSoundExposedParameter voiceGain;
    voiceGain.parameterId = "voiceGain";
    voiceGain.displayName = "Voice Gain";
    voiceGain.defaultValue.parameterId = "voiceGain";
    voiceGain.defaultValue.expression = "0.75";
    talas::project::ProjectSoundExposedParameter voiceMode;
    voiceMode.parameterId = "voiceMode";
    voiceMode.displayName = "Voice Mode";
    voiceMode.kind = talas::core::ParameterKind::Choice;
    voiceMode.choices = {"mono", "poly"};
    voiceMode.defaultValue.parameterId = "voiceMode";
    voiceMode.defaultValue.choice = "mono";
    talas::project::ProjectSoundExposedParameter voiceEnabled;
    voiceEnabled.parameterId = "voiceEnabled";
    voiceEnabled.displayName = "Voice Enabled";
    voiceEnabled.kind = talas::core::ParameterKind::Boolean;
    voiceEnabled.defaultValue.parameterId = "voiceEnabled";
    voiceEnabled.defaultValue.boolean = false;
    talas::project::ProjectSoundExposedParameter voiceSample;
    voiceSample.parameterId = "voiceSample";
    voiceSample.displayName = "Voice Sample";
    voiceSample.kind = talas::core::ParameterKind::Resource;
    voiceSample.defaultValue.parameterId = "voiceSample";
    voiceSample.defaultValue.resource = "voice.wav";

    talas::project::ProjectSoundDefinition voiceDefinition;
    voiceDefinition.definitionId = "voice-def";
    voiceDefinition.displayName = "Voice";
    voiceDefinition.exposedParameters = {
        voiceGain,
        voiceMode,
        voiceEnabled,
        voiceSample,
    };
    voiceDefinition.graph.nodes.push_back(
        {"inner-control", "talas.snapshotFixture", {}});
    talas::project::ProjectNode childInstance;
    childInstance.instanceId = "child";
    childInstance.soundDefinitionId = "child-def";
    voiceDefinition.graph.nodes.push_back(childInstance);

    talas::project::ProjectDocument document;
    document.soundDefinitions = {voiceDefinition, childDefinition};
    document.graph.nodes.push_back(
        {"root-control", "talas.snapshotFixture", {}});
    talas::project::ProjectNode voiceInstance;
    voiceInstance.instanceId = "voice";
    voiceInstance.soundDefinitionId = "voice-def";
    document.graph.nodes.push_back(voiceInstance);
    document.virtualControls.push_back({
        "Performance",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.6,
        0.1,
        1.0,
    });

    talas::project::ProjectVariableDeclaration globalGain;
    globalGain.variableId = "globalGain";
    globalGain.displayName = "Global Gain";
    globalGain.initialValue = 0.25;

    talas::project::ProjectVariableDeclaration voiceState;
    voiceState.variableId = "voiceState";
    voiceState.displayName = "Voice State";
    voiceState.domain = talas::expr::ValueDomain::Hertz;
    voiceState.initialValue = 440.0;
    voiceState.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    voiceState.owner.definitionId = "voice-def";

    talas::project::ProjectVariableDeclaration childState;
    childState.variableId = "childState";
    childState.displayName = "Child State";
    childState.initialValue = 0.75;
    childState.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    childState.owner.definitionId = "child-def";

    talas::project::ProjectVariableDeclaration privateVoice = voiceState;
    privateVoice.variableId = "privateVoice";
    privateVoice.displayName = "Private Voice";
    privateVoice.snapshotEligible = false;
    talas::project::ProjectVariableDeclaration localBias;
    localBias.variableId = "localBias";
    localBias.displayName = "Local Bias";
    localBias.initialValue = 0.1;
    localBias.owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;
    document.variableLibrary.variables = {
        globalGain,
        localBias,
        voiceState,
        childState,
        privateVoice,
    };
    return document;
}

const talas::project::SnapshotScopeTarget* findScopeTarget(
    const std::vector<talas::project::SnapshotScopeTarget>& targets,
    talas::project::SnapshotTargetKind kind,
    std::string_view ownerId,
    std::string_view parameterId)
{
    const auto found = std::ranges::find_if(
        targets,
        [&](const auto& target) {
            if (target.target.kind != kind)
                return false;
            if (kind == talas::project::SnapshotTargetKind::EventValue)
                return target.target.eventName == ownerId;
            if (kind == talas::project::SnapshotTargetKind::NodeParameter)
            {
                return target.target.nodeId == ownerId
                    && target.target.parameterId == parameterId;
            }
            if (kind == talas::project::SnapshotTargetKind::Variable)
            {
                const auto exactOwner = target.target.instancePath.empty()
                    ? std::string_view{}
                    : std::string_view{
                          target.target.instancePath.back()};
                return exactOwner == ownerId
                    && target.target.variableId == parameterId;
            }
            return target.target.instancePath.back() == ownerId
                && target.target.parameterId == parameterId;
        });
    return found == targets.end() ? nullptr : &*found;
}

void testScopeResolutionInitializationAndCaptureRespectSoundBoundaries()
{
    auto registry = makeRegistry();
    const auto document = makeScopedSnapshotDocument(registry);

    const auto project = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        {});
    require(project.ok() && project.targets.size() == 9,
            "project scope should expose root owners and Virtual Controls only");
    require(
        findScopeTarget(
            project.targets,
            talas::project::SnapshotTargetKind::NodeParameter,
            "root-control",
            "gain") != nullptr
            && findScopeTarget(
                project.targets,
                talas::project::SnapshotTargetKind::SoundParameter,
                "voice",
                "voiceGain") != nullptr
            && findScopeTarget(
                project.targets,
                talas::project::SnapshotTargetKind::EventValue,
                "Performance",
                {}) != nullptr
            && findScopeTarget(
                project.targets,
                talas::project::SnapshotTargetKind::Variable,
                {},
                "globalGain") != nullptr
            && std::ranges::any_of(
                project.targets,
                [](const auto& target) {
                    return target.target.kind
                            == talas::project::SnapshotTargetKind::Variable
                        && target.target.variableId == "localBias"
                        && target.target.variableOwnerKind
                            == talas::project::SnapshotVariableOwnerKind::WorkspaceSession;
                })
            && findScopeTarget(
                project.targets,
                talas::project::SnapshotTargetKind::Variable,
                "voice",
                "voiceState") == nullptr
            && findScopeTarget(
                project.targets,
                talas::project::SnapshotTargetKind::NodeParameter,
                "inner-control",
                "gain") == nullptr,
        "project scope must not pierce a root Sound boundary");
    require(
        std::ranges::none_of(
            project.targets,
            [](const auto& target) {
                return target.target.parameterId == "sample"
                    || target.target.parameterId == "label"
                    || target.target.parameterId == "voiceSample";
            }),
        "resource and text parameters must not become snapshot targets");

    talas::project::SnapshotScopeIdentity definitionScope;
    definitionScope.kind = talas::project::SnapshotScopeKind::Definition;
    definitionScope.instancePath = {"voice"};
    definitionScope.definitionId = "voice-def";
    const auto definition = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        definitionScope);
    require(definition.ok() && definition.targets.size() == 5,
            "definition scope should expose direct nodes and child Sound interfaces");
    const auto* innerGain = findScopeTarget(
        definition.targets,
        talas::project::SnapshotTargetKind::NodeParameter,
        "inner-control",
        "gain");
    const auto* childDepthTarget = findScopeTarget(
        definition.targets,
        talas::project::SnapshotTargetKind::SoundParameter,
        "child",
        "depth");
    require(
        innerGain != nullptr
            && innerGain->target.instancePath
                == std::vector<std::string>{"voice"}
            && innerGain->target.containingDefinitionId
                == std::optional<std::string>{"voice-def"}
            && childDepthTarget != nullptr
            && findScopeTarget(
                definition.targets,
                talas::project::SnapshotTargetKind::Variable,
                "voice",
                "voiceState") != nullptr
            && findScopeTarget(
                definition.targets,
                talas::project::SnapshotTargetKind::Variable,
                "voice",
                "privateVoice") == nullptr
            && childDepthTarget->target.instancePath
                == std::vector<std::string>{"voice", "child"}
            && talas::project::snapshotTargetIdentityString(
                   childDepthTarget->target)
                == talas::project::qualifiedSnapshotSoundParameterIdentity(
                    {"voice", "child"},
                    "child-def",
                    "depth")
            && findScopeTarget(
                definition.targets,
                talas::project::SnapshotTargetKind::NodeParameter,
                "hidden-child-node",
                "gain") == nullptr,
        "definition scope identities must be exact without piercing child Sounds");

    talas::project::SnapshotScopeIdentity nodeScope;
    nodeScope.kind = talas::project::SnapshotScopeKind::Node;
    nodeScope.nodeId = "voice";
    const auto node = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        nodeScope);
    require(node.ok() && node.targets.size() == 4,
            "Sound node scope should expose only its semantic interface");

    const auto allTargets = talas::project::resolveAllSnapshotTargets(
        document,
        registry);
    require(
        allTargets.ok() && allTargets.targets.size() == 18
            && findScopeTarget(
                allTargets.targets,
                talas::project::SnapshotTargetKind::NodeParameter,
                "hidden-child-node",
                "gain") != nullptr,
        "complete target discovery should recurse through concrete Sound instances exactly once");

    const auto initialized =
        talas::project::initializeSnapshotScopeValues(project.targets);
    require(initialized.ok() && initialized.values.size() == 9,
            "all eligible project targets should initialize from semantic defaults");
    const auto event = std::ranges::find_if(
        initialized.values,
        [](const auto& entry) {
            return entry.target.kind
                    == talas::project::SnapshotTargetKind::EventValue
                && entry.target.eventName == "Performance";
        });
    require(
        event != initialized.values.end()
            && event->value.number == 0.6,
        "Virtual Control initialization should use its persisted value");
    const auto* eventDescriptor = findScopeTarget(
        project.targets,
        talas::project::SnapshotTargetKind::EventValue,
        "Performance",
        {});
    require(
        eventDescriptor != nullptr
            && eventDescriptor->initialRandomRange
                == talas::project::SnapshotParameterRange{0.1, 1.0},
        "Virtual Control range should initialize its randomization policy");

    auto current = initialized.values;
    std::ranges::reverse(current);
    const auto captured = talas::project::captureSnapshotScopeValues(
        project.targets,
        current);
    require(
        captured.ok() && captured.values == initialized.values,
        "capture should accept a complete current set and canonicalize its order");
    current.pop_back();
    const auto incomplete = talas::project::captureSnapshotScopeValues(
        project.targets,
        current);
    require(
        !incomplete.ok() && incomplete.values.empty(),
        "capture must reject a missing current target transactionally");

    auto unavailableDefaults = project.targets;
    unavailableDefaults.front().defaultValue.reset();
    unavailableDefaults.front().defaultUnavailableReason =
        "Fixture default is intentionally unavailable";
    const auto rejectedInitialization =
        talas::project::initializeSnapshotScopeValues(unavailableDefaults);
    require(
        !rejectedInitialization.ok()
            && rejectedInitialization.values.empty()
            && rejectedInitialization.diagnostics.front().targetIdentity
                == talas::project::snapshotTargetIdentityString(
                    unavailableDefaults.front().target),
        "one unavailable semantic default should reject initialization atomically");

    auto mismatchedCurrent = initialized.values;
    mismatchedCurrent.front().value.kind =
        talas::project::SnapshotValueKind::Boolean;
    const auto mismatchedCapture =
        talas::project::captureSnapshotScopeValues(
            project.targets,
            mismatchedCurrent);
    require(
        !mismatchedCapture.ok() && mismatchedCapture.values.empty(),
        "one changed current value kind should reject capture atomically");

    auto mismatchedScopeDocument = document;
    talas::project::ProjectSnapshot mismatchedSnapshot;
    mismatchedSnapshot.snapshotId = "mismatched";
    mismatchedSnapshot.displayName = "Mismatched";
    mismatchedSnapshot.values.push_back(*event);
    talas::project::ProjectSnapshotBank mismatchedBank;
    mismatchedBank.bankId = "node-bank";
    mismatchedBank.displayName = "Node Bank";
    mismatchedBank.scope.kind = talas::project::SnapshotScopeKind::Node;
    mismatchedBank.scope.nodeId = "root-control";
    mismatchedBank.snapshots.push_back(mismatchedSnapshot);
    mismatchedScopeDocument.snapshotLibrary.banks.push_back(mismatchedBank);
    const auto scopeDiagnostics = talas::project::snapshotLibraryDiagnostics(
        mismatchedScopeDocument,
        registry);
    require(
        scopeDiagnostics.size() == 1
            && scopeDiagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::ScopeMismatch,
        "a valid target outside its bank scope should remain portable but diagnostic");
}

void testCompleteTargetDiscoveryUsesItsOwnBoundedCapacity()
{
    talas::project::ProjectDocument document;
    for (std::size_t index = 0; index < 300; ++index)
    {
        document.graph.nodes.push_back({
            "osc-" + std::to_string(index),
            "talas.oscillator",
            {},
        });
    }
    const auto registry = makeRegistry();
    const auto projectScope =
        talas::project::resolveSnapshotScopeTargets(
            document,
            registry,
            {});
    require(
        !projectScope.ok()
            && projectScope.targets.empty()
            && projectScope.diagnostics.front().code
                == talas::project::SnapshotDiagnosticCode::CapacityExceeded,
        "one persisted snapshot scope should keep the 512-value schema bound");

    const auto complete = talas::project::resolveAllSnapshotTargets(
        document,
        registry);
    require(
        complete.ok()
            && complete.targets.size()
                > talas::project::maximumSnapshotValuesPerSnapshot
            && complete.targets.size()
                <= talas::project::maximumResolvedSnapshotTargetCount,
        "prepared target discovery should use its independent 8192-target universe bound");
}

void testCanonicalTargetIdentityGrammarMatchesTheStructuralEncoder()
{
    auto event = eventTarget("Cutoff");

    talas::project::SnapshotTargetIdentity node;
    node.kind = talas::project::SnapshotTargetKind::NodeParameter;
    node.instancePath = {"voice_a"};
    node.containingDefinitionId = "voice_def";
    node.nodeId = "filter";
    node.parameterId = "cutoff";

    talas::project::SnapshotTargetIdentity sound;
    sound.kind = talas::project::SnapshotTargetKind::SoundParameter;
    sound.instancePath = {"voice_a"};
    sound.definitionId = "voice_def";
    sound.parameterId = "gain";

    talas::project::SnapshotTargetIdentity projectVariable;
    projectVariable.kind = talas::project::SnapshotTargetKind::Variable;
    projectVariable.variableId = "globalGain";

    talas::project::SnapshotTargetIdentity soundVariable;
    soundVariable.kind = talas::project::SnapshotTargetKind::Variable;
    soundVariable.instancePath = {"voice_a"};
    soundVariable.definitionId = "voice_def";
    soundVariable.variableId = "voiceState";

    talas::project::SnapshotTargetIdentity workspaceVariable;
    workspaceVariable.kind = talas::project::SnapshotTargetKind::Variable;
    workspaceVariable.variableOwnerKind =
        talas::project::SnapshotVariableOwnerKind::WorkspaceSession;
    workspaceVariable.variableId = "localBias";

    for (const auto* target : {
             &event,
             &node,
             &sound,
             &projectVariable,
             &soundVariable,
             &workspaceVariable,
         })
    {
        require(
            talas::project::isCanonicalSnapshotTargetIdentityString(
                talas::project::snapshotTargetIdentityString(*target)),
            "every structural target encoder variant should satisfy the canonical grammar");
    }

    for (const std::string_view malformed : {
             "",
             "target-by-index-7",
             "d0:/n01:x/p1:y",
             "d0:/n1:x/p2:y",
             "e1:x/u7:unknown",
             "i0:/d1:d/p1:p",
             "e3:a-b/u6:scalar",
             "d1:d/n1:n/p1:p",
             "i1:x/d0:/n1:n/p1:p",
             "d1:d/v1:v",
             "i1:x/d0:/v1:v",
             "v3:a-b",
             "d1:d/p1:p",
             "d1:d/p1:p/trailing",
         })
    {
        require(
            !talas::project::isCanonicalSnapshotTargetIdentityString(
                malformed),
            "malformed structural target identity should fail closed");
    }

    const auto oversizedIdentifier = std::string(
        talas::project::maximumSnapshotIdentifierBytes + 1, 'x');
    require(
        !talas::project::isCanonicalSnapshotTargetIdentityString(
            "d0:/n" + std::to_string(oversizedIdentifier.size()) + ":"
            + oversizedIdentifier + "/p1:p"),
        "structural target segments should share the snapshot identifier byte limit");
}

void testSoundVariableSnapshotsStoreExactCandidateIdentity()
{
    auto registry = makeRegistry();
    talas::project::ProjectDocument document;
    document.soundDefinitions = {
        {.definitionId = "toneA", .displayName = "Tone A"},
        {.definitionId = "toneB", .displayName = "Tone B"},
        {.definitionId = "container", .displayName = "Container"},
    };
    talas::project::ProjectNode instance;
    instance.instanceId = "voice";
    instance.soundDefinitionId = "container";
    document.graph.nodes.push_back(instance);

    talas::project::ProjectSoundVariableDeclaration projectValue;
    projectValue.variableId = "selectedTone";
    projectValue.displayName = "Selected Tone";
    projectValue.candidateDefinitionIds = {"toneA", "toneB"};
    projectValue.initialSelectedOrdinal = 1;
    projectValue.snapshotEligible = true;

    auto localValue = projectValue;
    localValue.variableId = "localTone";
    localValue.owner.kind =
        talas::project::ProjectVariableOwnerKind::WorkspaceSession;

    auto instanceValue = projectValue;
    instanceValue.variableId = "voiceTone";
    instanceValue.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    instanceValue.owner.definitionId = "container";
    document.soundVariableLibrary.variables = {
        projectValue,
        localValue,
        instanceValue,
    };

    const auto project = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        {});
    require(project.ok(), "project Sound-variable targets should resolve");
    const auto selected = std::ranges::find_if(
        project.targets,
        [](const auto& target) {
            return target.target.kind
                    == talas::project::SnapshotTargetKind::SoundVariable
                && target.target.variableId == "selectedTone";
        });
    const auto local = std::ranges::find_if(
        project.targets,
        [](const auto& target) {
            return target.target.kind
                    == talas::project::SnapshotTargetKind::SoundVariable
                && target.target.variableId == "localTone";
        });
    require(
        selected != project.targets.end()
            && selected->valueKind
                == talas::project::SnapshotValueKind::Choice
            && selected->choices
                == std::vector<std::string>{"toneA", "toneB"}
            && selected->defaultValue.has_value()
            && selected->defaultValue->choice == "toneB"
            && local != project.targets.end()
            && local->target.variableOwnerKind
                == talas::project::SnapshotVariableOwnerKind::WorkspaceSession,
        "project and workspace Sound-variable targets should preserve exact structural choices");

    talas::project::SnapshotScopeIdentity nodeScope;
    nodeScope.kind = talas::project::SnapshotScopeKind::Node;
    nodeScope.nodeId = "voice";
    const auto node = talas::project::resolveSnapshotScopeTargets(
        document,
        registry,
        nodeScope);
    const auto voice = std::ranges::find_if(
        node.targets,
        [](const auto& target) {
            return target.target.kind
                    == talas::project::SnapshotTargetKind::SoundVariable
                && target.target.variableId == "voiceTone";
        });
    require(
        node.ok() && voice != node.targets.end()
            && voice->target.instancePath
                == std::vector<std::string>{"voice"}
            && voice->target.definitionId == "container",
        "Sound-owned Sound-variable snapshot targets should bind to the exact instance path");

    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "sound-state";
    snapshot.displayName = "Sound State";
    snapshot.values.push_back({selected->target, *selected->defaultValue});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "project";
    bank.displayName = "Project";
    bank.snapshots.push_back(std::move(snapshot));
    document.snapshotLibrary.banks.push_back(std::move(bank));
    const auto encoded = talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(encoded);
    require(
        loaded.ok()
            && loaded.document.snapshotLibrary == document.snapshotLibrary
            && encoded.find("\"kind\": \"soundVariable\"")
                != std::string::npos,
        "format 29 should round-trip exact Sound-variable snapshot identities");

    document.soundVariableLibrary.variables.front()
        .candidateDefinitionIds = {"toneA"};
    const auto diagnostics = talas::project::snapshotLibraryDiagnostics(
        document,
        registry);
    require(
        std::ranges::any_of(
            diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code
                    == talas::project::SnapshotDiagnosticCode::InvalidChoice;
            }),
        "snapshot recall must reject a selected Sound identity removed from the candidate set");
}
}

// Talas keeps the layouts and presets of controls
// and Sounds the project no longer has, in case they are revived; Save as...
// offers to remove them. What is obsolete: the widgets kept for controls the
// surface does not have, the layouts' places for them (a layout with only
// those going too), and the presets' values and freezes of a removed Sound,
// parameter or Event Value; a target that has only changed stays.
void testObsoleteLayoutsAndPresetsAreRemovedOnRequest()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"osc-main", "talas.oscillator", {}});
    talas::project::reconcileProjectPresentation(document);
    document.virtualControls.push_back(
        {"Cutoff", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    document.virtualControls.push_back(
        {"Vol", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});
    document.dormantVirtualControls.push_back(
        {"Gone", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.0, 0.0, 1.0});
    document.virtualControlLayoutPages = {{"Main", {"Cutoff", "Gone"}}, {"Old", {"Gone"}}, {"Blank", {}}};
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "vcsPresets";
    bank.displayName = "Presets";
    bank.parameterPolicies = {{eventTarget("Cutoff"), true, std::nullopt}, {eventTarget("Gone"), true, std::nullopt}};
    auto changed = eventTarget("Vol");
    changed.domain = talas::expr::ValueDomain::Hertz;
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "loud";
    snapshot.displayName = "Loud";
    snapshot.values = {numericValue(eventTarget("Cutoff"), 0.75), numericValue(eventTarget("Gone"), 0.2),
        numericNodeValue("osc-main", "removedFrequency", 440.0), numericNodeValue("removed-osc", "frequency", 220.0),
        numericValue(changed, 3.0)};
    bank.snapshots.push_back(snapshot);
    document.snapshotLibrary.banks.push_back(bank);

    const auto registry = makeRegistry();
    const auto found = talas::project::obsoleteVirtualControlMaterial(document, registry);
    require(found.widgets == 1 && found.layoutEntries == 2 && found.presetValues == 4,
        "the obsolete widget, layout places and preset values should be found, a changed target not among them");
    auto cleaned = document;
    require(talas::project::removeObsoleteVirtualControlMaterial(cleaned, registry) == found,
        "removing should answer what it removed");
    const auto& kept = cleaned.snapshotLibrary.banks.front();
    require(cleaned.dormantVirtualControls.empty() && cleaned.virtualControls.size() == 2
            && cleaned.virtualControlLayoutPages
                == std::vector<talas::project::VirtualControlLayoutPage>{{"Main", {"Cutoff"}}, {"Blank", {}}},
        "the obsolete widget and layout places should go, a layout left without controls with them");
    require(kept.parameterPolicies.size() == 1 && kept.parameterPolicies.front().target == eventTarget("Cutoff")
            && kept.snapshots.front().values
                == std::vector<talas::project::SnapshotValueEntry>{numericValue(eventTarget("Cutoff"), 0.75),
                    numericValue(changed, 3.0)},
        "the presets should keep their live values and the changed one, the obsolete ones gone");
    auto again = cleaned;
    require(talas::project::obsoleteVirtualControlMaterial(cleaned, registry).empty()
            && talas::project::removeObsoleteVirtualControlMaterial(again, registry).empty()
            && talas::project::serializeProjectDocument(again) == talas::project::serializeProjectDocument(cleaned),
        "nothing obsolete should be left to remove");
}

int main()
{
    testMissingParameterRemainsPortableAndProducesDiagnostic();
    testSoundParameterAliasIsReportedButNeverAppliedSilently();
    testMorphUsesCanonicalTargetsLocksAndExactDiscreteRule();
    testSeededRandomizationIsExactAndLockStable();
    testOperationDiagnosticsAreBoundedWithOneTruncationMarker();
    testScopeResolutionInitializationAndCaptureRespectSoundBoundaries();
    testCompleteTargetDiscoveryUsesItsOwnBoundedCapacity();
    testCanonicalTargetIdentityGrammarMatchesTheStructuralEncoder();
    testSoundVariableSnapshotsStoreExactCandidateIdentity();
    testObsoleteLayoutsAndPresetsAreRemovedOnRequest();
    return 0;
}
