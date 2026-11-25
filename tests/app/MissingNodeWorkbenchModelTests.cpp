#include "talas/app/MissingNodeWorkbenchModel.h"
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

talas::project::ProjectPreservedComponent preservedComponent()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    const auto monoAudio = talas::core::PortSignalContract{
        talas::core::PortSignalRate::Sample,
        talas::core::AudioBusLayoutKind::Mono,
        talas::core::PortEdgeSemantics::Continuous,
        talas::core::SpectralFrameFormat::NotApplicable,
        true,
    };
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        monoAudio,
    });
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        monoAudio,
    });
    component.parameters.push_back({
        "mode",
        talas::core::ParameterKind::Choice,
        talas::expr::ValueDomain::Scalar,
        {"clean", "colour"},
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    return component;
}

talas::project::ProjectDocument rootDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    missing.preservedComponent = preservedComponent();
    document.graph.nodes = {
        {"osc", "talas.oscillator", {}},
        std::move(missing),
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "missing1", "audioIn"},
        {"missing1", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::core::NodeTypeDescriptor matchingDescriptor()
{
    auto descriptor =
        talas::project::makeProjectPreservedComponentDescriptor(
            "org.example.component",
            preservedComponent());
    descriptor.typeVersion = "1.2.3";
    descriptor.stateSchemaVersion = 4;
    return descriptor;
}

void testMissingProjectionIsHonestAccessibleAndActionable()
{
    const auto document = rootDocument();
    const auto projection =
        talas::app::missingNodeWorkbenchProjection(
            document,
            std::nullopt,
            "missing1",
            talas::core::makeBuiltinNodeRegistry());
    require(projection.has_value(), "preserved node should project");
    require(
        projection->availability
                == talas::project::ProjectComponentAvailability::Missing
            && projection->qualifiedNodeId == "missing1"
            && projection->displayName == "Unavailable example"
            && projection->statusLabel == "Missing"
            && projection->typeVersionText
                == "org.example.component @ 1.2.3"
            && projection->stateText
                == "State schema 4 · 3 bytes"
            && projection->bundleText
                == "org.example.component"
            && projection->recoveryText
                == "Install the matching owner-approved bundle",
        "projection should expose exact preserved identity, state, and recovery copy");
    require(
        projection->ports.size() == 2
            && projection->ports[0].direction
                == talas::app::MissingNodePortDirection::Input
            && projection->ports[0].required
            && projection->ports[0].connected
            && projection->ports[1].direction
                == talas::app::MissingNodePortDirection::Output
            && projection->ports[1].connected,
        "projection should retain frozen port order and connection state");
    require(
        projection->canRetry
            && projection->canDisconnect
            && projection->canDelete
            && !projection->parameterEditingEnabled,
        "unresolved projection should expose only safe actions");
    require(
        projection->accessibleName.find("Missing")
                != std::string::npos
            && projection->accessibleDescription.find(
                   "3 bytes")
                != std::string::npos
            && projection->ports.front()
                   .accessibleDescription.find("connected")
                != std::string::npos,
        "status and port state should be available without relying on colour");
}

void testMismatchAndResolvedStatesRemainExact()
{
    const auto document = rootDocument();

    auto versionRegistry = talas::core::makeBuiltinNodeRegistry();
    auto version = matchingDescriptor();
    version.typeVersion = "2.0.0";
    versionRegistry.addNodeType(std::move(version));
    const auto versionProjection =
        talas::app::missingNodeWorkbenchProjection(
            document,
            std::nullopt,
            "missing1",
            versionRegistry);
    require(
        versionProjection->statusLabel
                == "Incompatible version"
            && versionProjection->diagnosticText.find(
                   "type version mismatch")
                != std::string::npos,
        "type version mismatch should retain its exact reason");

    auto stateRegistry = talas::core::makeBuiltinNodeRegistry();
    auto state = matchingDescriptor();
    state.stateSchemaVersion = 5;
    stateRegistry.addNodeType(std::move(state));
    const auto stateProjection =
        talas::app::missingNodeWorkbenchProjection(
            document,
            std::nullopt,
            "missing1",
            stateRegistry);
    require(
        stateProjection->statusLabel
                == "Incompatible state"
            && stateProjection->diagnosticText.find(
                   "state schema mismatch")
                != std::string::npos,
        "state mismatch should remain distinct from type version mismatch");

    auto interfaceRegistry = talas::core::makeBuiltinNodeRegistry();
    auto interface = matchingDescriptor();
    interface.outputs.front().id = "renamedOut";
    interfaceRegistry.addNodeType(std::move(interface));
    const auto interfaceProjection =
        talas::app::missingNodeWorkbenchProjection(
            document,
            std::nullopt,
            "missing1",
            interfaceRegistry);
    require(
        interfaceProjection->statusLabel
                == "Incompatible interface"
            && interfaceProjection->diagnosticText.find(
                   "output interface mismatch")
                != std::string::npos,
        "interface mismatch should retain its first exact reason");

    auto resolvedRegistry = talas::core::makeBuiltinNodeRegistry();
    resolvedRegistry.addNodeType(matchingDescriptor());
    const auto resolved =
        talas::app::missingNodeWorkbenchProjection(
            document,
            std::nullopt,
            "missing1",
            resolvedRegistry);
    require(
        resolved->statusLabel == "Resolved"
            && !resolved->canRetry
            && resolved->parameterEditingEnabled
            && resolved->canDelete,
        "exact trusted descriptor should refresh the same projection into an editable resolved state");
}

void testNestedScopeCompactLayoutAndKeyboardOrder()
{
    auto root = rootDocument();
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph = std::move(root.graph);
    document.soundDefinitions.push_back(std::move(definition));

    const auto projection =
        talas::app::missingNodeWorkbenchProjection(
            document,
            "voice",
            "missing1",
            talas::core::makeBuiltinNodeRegistry());
    require(
        projection.has_value()
            && projection->qualifiedNodeId == "voice/missing1",
        "nested projection should retain its Sound scope");
    require(
        talas::app::missingNodeLayoutMode(279)
                == talas::app::MissingNodeLayoutMode::Compact
            && talas::app::missingNodeLayoutMode(280)
                == talas::app::MissingNodeLayoutMode::Regular,
        "layout threshold should be deterministic");
    const auto order =
        talas::app::missingNodeKeyboardOrder(*projection);
    require(
        order
            == std::vector<talas::app::MissingNodeFocusTarget>{
                talas::app::MissingNodeFocusTarget::Retry,
                talas::app::MissingNodeFocusTarget::Disconnect,
                talas::app::MissingNodeFocusTarget::Delete,
                talas::app::MissingNodeFocusTarget::Diagnostic,
            },
        "keyboard order should include only enabled actions in task order");
}

void testOrdinaryAndUnknownSelectionsDoNotMasqueradeAsPreserved()
{
    const auto document = rootDocument();
    require(
        !talas::app::missingNodeWorkbenchProjection(
             document,
             std::nullopt,
             "osc",
             talas::core::makeBuiltinNodeRegistry())
             .has_value()
            && !talas::app::missingNodeWorkbenchProjection(
                    document,
                    std::nullopt,
                    "unknown",
                    talas::core::makeBuiltinNodeRegistry())
                    .has_value(),
        "ordinary and unknown selections should not invent preserved-component UI state");
}
}

int main()
{
    testMissingProjectionIsHonestAccessibleAndActionable();
    testMismatchAndResolvedStatesRemainExact();
    testNestedScopeCompactLayoutAndKeyboardOrder();
    testOrdinaryAndUnknownSelectionsDoNotMasqueradeAsPreserved();
    return 0;
}
