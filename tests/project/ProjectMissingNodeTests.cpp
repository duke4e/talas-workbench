#include "talas/project/ProjectMissingNodes.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testOpaqueStateUsesCanonicalBase64()
{
    const std::vector<std::uint8_t> bytes{
        0x00, 0x01, 0x02, 0x7f, 0x80, 0xfe, 0xff};
    const auto encoded =
        talas::project::encodeProjectComponentState(bytes);
    require(
        encoded == "AAECf4D+/w==",
        "opaque component bytes should use canonical padded base64");

    const auto decoded =
        talas::project::decodeProjectComponentState(encoded);
    require(decoded.ok(), "canonical component state should decode");
    require(
        decoded.bytes == bytes,
        "component-state base64 should preserve arbitrary bytes exactly");

    const auto bounded =
        talas::project::decodeProjectComponentState(encoded, bytes.size() - 1U);
    require(
        !bounded.ok() && bounded.bytes.empty()
            && bounded.error.find("limit") != std::string::npos,
        "bounded decoding should reject before retaining bytes beyond the caller budget");

    for (const auto invalid : {
             "AAECf4D+/w",
             "AAECf4D+/w=",
             "AAECf4D+/w===",
             "AAECf4D-_w==",
             "AAECf4D+/x==",
             "A===",
         })
    {
        const auto rejected =
            talas::project::decodeProjectComponentState(invalid);
        require(
            !rejected.ok() && !rejected.error.empty(),
            "non-canonical or malformed base64 should fail closed");
    }
}

void testPreservedComponentValidationIsBoundedAndStructural()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        {
            talas::core::PortSignalRate::Sample,
            talas::core::AudioBusLayoutKind::Stereo,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::NotApplicable,
            true,
        },
    });
    component.outputs.push_back({
        "spectrumOut",
        talas::core::PortType::ComplexSpectrum,
        false,
        {
            talas::core::PortSignalRate::Frame,
            talas::core::AudioBusLayoutKind::NotApplicable,
            talas::core::PortEdgeSemantics::NotApplicable,
            talas::core::SpectralFrameFormat::ComplexSpectrumV1,
            true,
        },
    });
    component.parameters.push_back({
        "mode",
        talas::core::ParameterKind::Choice,
        talas::expr::ValueDomain::Scalar,
        {"clean", "colour"},
    });
    component.recoveryHint = "Install the matching owner-approved bundle";

    require(
        !talas::project::validateProjectPreservedComponent(component)
             .has_value(),
        "well-formed preserved component should pass structural validation");

    auto duplicatePort = component;
    duplicatePort.outputs.push_back(duplicatePort.outputs.front());
    require(
        talas::project::validateProjectPreservedComponent(duplicatePort)
            .has_value(),
        "duplicate frozen port identities should fail validation");

    auto duplicateParameter = component;
    duplicateParameter.parameters.push_back(
        duplicateParameter.parameters.front());
    require(
        talas::project::validateProjectPreservedComponent(
            duplicateParameter)
            .has_value(),
        "duplicate frozen parameter identities should fail validation");

    auto oversized = component;
    oversized.state.resize(
        talas::project::maximumProjectPreservedComponentStateBytes + 1);
    require(
        talas::project::validateProjectPreservedComponent(oversized)
            .has_value(),
        "oversized opaque component state should fail before publication");
}

talas::project::ProjectPreservedComponent preservedComponent()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        {
            talas::core::PortSignalRate::Sample,
            talas::core::AudioBusLayoutKind::Stereo,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::NotApplicable,
            true,
        },
    });
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        {
            talas::core::PortSignalRate::Sample,
            talas::core::AudioBusLayoutKind::Stereo,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::NotApplicable,
            true,
        },
    });
    component.parameters.push_back({
        "mode",
        talas::core::ParameterKind::Choice,
        talas::expr::ValueDomain::Scalar,
        {"clean", "colour"},
    });
    component.recoveryHint = "Install the matching owner-approved bundle";
    return component;
}

talas::project::ProjectPreservedComponent minimalPreservedComponent()
{
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.0.0";
    component.stateSchemaVersion = 1;
    component.displayName = "Unavailable minimal component";
    component.bundleId = "org.example.minimal";
    component.recoveryHint = "Install the matching owner-approved bundle";
    return component;
}

talas::core::NodeTypeDescriptor matchingDescriptor(
    const talas::project::ProjectPreservedComponent& component)
{
    auto descriptor =
        talas::project::makeProjectPreservedComponentDescriptor(
            "org.example.component",
            component);
    descriptor.typeVersion = component.typeVersion;
    descriptor.stateSchemaVersion = component.stateSchemaVersion;
    return descriptor;
}

void testAvailabilityRequiresAnExactTrustedDescriptor()
{
    const auto component = preservedComponent();
    talas::core::NodeRegistry emptyRegistry;

    const auto legacyUnknown =
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            nullptr,
            emptyRegistry);
    require(
        legacyUnknown.availability
            == talas::project::ProjectComponentAvailability::LegacyUnknown
            && !legacyUnknown.executable(),
        "unknown legacy nodes without a capsule should remain invalid");

    const auto missing =
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            &component,
            emptyRegistry);
    require(
        missing.availability
            == talas::project::ProjectComponentAvailability::Missing
            && !missing.executable()
            && missing.message.find("org.example.component")
                != std::string::npos,
        "a valid capsule without a descriptor should be preserved as missing");

    talas::core::NodeRegistry matchingRegistry;
    matchingRegistry.addNodeType(matchingDescriptor(component));
    const auto native =
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            nullptr,
            matchingRegistry);
    require(
        native.availability
            == talas::project::ProjectComponentAvailability::Native
            && native.executable(),
        "registered ordinary nodes should retain native availability");
    const auto resolved =
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            &component,
            matchingRegistry);
    require(
        resolved.availability
            == talas::project::ProjectComponentAvailability::Resolved
            && resolved.executable(),
        "an exact trusted descriptor should resolve the preserved component");

    auto wrongVersion = matchingDescriptor(component);
    wrongVersion.typeVersion = "1.2.4";
    talas::core::NodeRegistry wrongVersionRegistry;
    wrongVersionRegistry.addNodeType(std::move(wrongVersion));
    require(
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            &component,
            wrongVersionRegistry)
                .availability
            == talas::project::ProjectComponentAvailability::IncompatibleVersion,
        "component type-version mismatches should fail distinctly");

    auto wrongState = matchingDescriptor(component);
    wrongState.stateSchemaVersion = 5;
    talas::core::NodeRegistry wrongStateRegistry;
    wrongStateRegistry.addNodeType(std::move(wrongState));
    require(
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            &component,
            wrongStateRegistry)
                .availability
            == talas::project::ProjectComponentAvailability::IncompatibleStateSchema,
        "component state-schema mismatches should fail distinctly");

    auto wrongInterface = matchingDescriptor(component);
    wrongInterface.outputs.front().id = "renamedOut";
    talas::core::NodeRegistry wrongInterfaceRegistry;
    wrongInterfaceRegistry.addNodeType(std::move(wrongInterface));
    const auto incompatible =
        talas::project::classifyProjectComponentAvailability(
            "org.example.component",
            &component,
            wrongInterfaceRegistry);
    require(
        incompatible.availability
            == talas::project::ProjectComponentAvailability::IncompatibleInterface
            && incompatible.message.find("output") != std::string::npos,
        "frozen interface mismatches should identify the first contract family");
}

talas::project::ProjectDocument preservedGraphDocument()
{
    auto component = preservedComponent();
    component.inputs.front().signal.audioLayout =
        talas::core::AudioBusLayoutKind::Mono;
    component.outputs.front().signal.audioLayout =
        talas::core::AudioBusLayoutKind::Mono;

    talas::project::ProjectDocument document;
    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    missing.preservedComponent = std::move(component);
    talas::project::ProjectNode output;
    output.instanceId = "out";
    output.typeId = "talas.audioOutput";
    document.graph.nodes = {
        std::move(oscillator),
        std::move(missing),
        std::move(output),
    };
    document.graph.connections = {
        {"osc", "audioOut", "missing1", "audioIn"},
        {"missing1", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    return document;
}

void testStructuralValidationPreservesButExecutionRejectsMissingNodes()
{
    const auto document = preservedGraphDocument();
    const auto builtins = talas::core::makeBuiltinNodeRegistry();
    const auto structural =
        talas::project::validateProjectDocumentForEditing(
            document,
            builtins);
    require(
        structural.ok(),
        "editing validation should prove topology through the frozen interface");
    require(
        !talas::project::describeProjectNode(
             document,
             document.graph.nodes[1],
             builtins)
             .has_value(),
        "ordinary descriptor lookup should never invent an executable placeholder");
    const auto frozenDescriptor =
        talas::project::describeProjectNodeForEditing(
            document,
            document.graph.nodes[1],
            builtins);
    require(
        frozenDescriptor.has_value()
            && frozenDescriptor->typeId == "org.example.component"
            && frozenDescriptor->inputs.front().id == "audioIn"
            && frozenDescriptor->outputs.front().id == "audioOut"
            && frozenDescriptor->capabilities.offline
                == talas::core::ExecutionSupport::Unsupported,
        "editing descriptor lookup should expose only the frozen non-executable interface");

    const auto executable =
        talas::project::validateProjectDocument(document, builtins);
    require(
        !executable.ok()
            && executable.errors.front().nodeId == "missing1"
            && executable.errors.front().message.find("Missing component type")
                != std::string::npos,
        "execution validation should reject the missing component with scope");

    auto matchingRegistry = talas::core::makeBuiltinNodeRegistry();
    matchingRegistry.addNodeType(
        matchingDescriptor(
            *document.graph.nodes[1].preservedComponent));
    require(
        talas::project::validateProjectDocument(
            document,
            matchingRegistry)
            .ok(),
        "an exact trusted descriptor should make the preserved graph executable");

    auto legacyUnknown = document;
    legacyUnknown.graph.nodes[1].preservedComponent.reset();
    require(
        !talas::project::validateProjectDocumentForEditing(
             legacyUnknown,
             builtins)
             .ok(),
        "editing validation should still reject unknown legacy nodes without proof");

    auto incompatibleRegistry = talas::core::makeBuiltinNodeRegistry();
    auto incompatible = matchingDescriptor(
        *document.graph.nodes[1].preservedComponent);
    incompatible.typeVersion = "2.0.0";
    incompatibleRegistry.addNodeType(std::move(incompatible));
    require(
        talas::project::validateProjectDocumentForEditing(
            document,
            incompatibleRegistry)
            .ok(),
        "editing validation should retain topology through an incompatible installed version");
    const auto incompatibleExecution =
        talas::project::validateProjectDocument(
            document,
            incompatibleRegistry);
    require(
        !incompatibleExecution.ok()
            && incompatibleExecution.errors.front().message.find(
                   "type version mismatch")
                != std::string::npos,
        "execution validation should distinguish installed version mismatch");
}

void testNestedSoundPreservationRetainsQualifiedScope()
{
    auto component = preservedComponent();
    component.inputs.front().signal.audioLayout =
        talas::core::AudioBusLayoutKind::Mono;
    component.outputs.front().signal.audioLayout =
        talas::core::AudioBusLayoutKind::Mono;

    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "missing1",
        "audioIn",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "missing1",
        "audioOut",
    });
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    missing.preservedComponent = std::move(component);
    definition.graph.nodes.push_back(std::move(missing));
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "osc";
    oscillator.typeId = "talas.oscillator";
    talas::project::ProjectNode voice;
    voice.instanceId = "voice1";
    voice.soundDefinitionId = "voice";
    talas::project::ProjectNode output;
    output.instanceId = "out";
    output.typeId = "talas.audioOutput";
    document.graph.nodes = {
        std::move(oscillator),
        std::move(voice),
        std::move(output),
    };
    document.graph.connections = {
        {"osc", "audioOut", "voice1", "audioIn"},
        {"voice1", "audioOut", "out", "audioIn"},
    };
    talas::project::reconcileProjectPresentation(document);

    const auto builtins = talas::core::makeBuiltinNodeRegistry();
    require(
        talas::project::validateProjectDocumentForEditing(document, builtins)
            .ok(),
        "editing validation should preserve a missing node inside a reusable Sound");
    const auto executable =
        talas::project::validateProjectDocument(document, builtins);
    require(
        !executable.ok()
            && executable.errors.front().nodeId == "voice/missing1"
            && executable.errors.front().message.find("Missing component type")
                != std::string::npos,
        "execution validation should qualify a nested missing node by Sound scope");
}

void testEditingValidationAvoidsSyntheticTypeIdentityCollisions()
{
    const auto component = minimalPreservedComponent();
    auto registry = talas::core::makeBuiltinNodeRegistry();
    auto registeredCollision =
        talas::project::makeProjectPreservedComponentDescriptor(
            "talas.preserved.0",
            component);
    registry.addNodeType(std::move(registeredCollision));

    talas::project::ProjectDocument document;
    talas::project::ProjectNode missing;
    missing.instanceId = "missing";
    missing.typeId = "org.example.unavailable";
    missing.preservedComponent = component;
    document.graph.nodes.push_back(std::move(missing));
    talas::project::reconcileProjectPresentation(document);
    require(
        talas::project::validateProjectDocumentForEditing(document, registry)
            .ok(),
        "a registered talas.preserved.0 type must not collide with an internal preservation descriptor");

    talas::project::ProjectDocument legacyCollision;
    legacyCollision.graph.nodes.push_back({
        "legacy",
        "talas.preserved.0",
        {},
        {},
        {},
    });
    talas::project::ProjectNode separateMissing;
    separateMissing.instanceId = "missing";
    separateMissing.typeId = "org.example.unavailable";
    separateMissing.preservedComponent = component;
    legacyCollision.graph.nodes.push_back(std::move(separateMissing));
    talas::project::reconcileProjectPresentation(legacyCollision);
    require(
        !talas::project::validateProjectDocumentForEditing(
             legacyCollision,
             talas::core::makeBuiltinNodeRegistry())
             .ok(),
        "a legacy unknown node must not resolve through another capsule's synthetic descriptor");
}

void testParserRejectsPreservedComponentCountBeforeRetainingTheNextCapsule()
{
    talas::project::ProjectDocument document;
    const auto component = minimalPreservedComponent();
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "preservedLibrary";
    definition.displayName = "Preserved Library";
    definition.graph.nodes.reserve(
        talas::project::maximumProjectPreservedComponents);
    for (std::size_t index = 0;
         index < talas::project::maximumProjectPreservedComponents;
         ++index)
    {
        talas::project::ProjectNode node;
        node.instanceId = "missing" + std::to_string(index);
        node.typeId = "org.example.unavailable";
        node.preservedComponent = component;
        definition.graph.nodes.push_back(std::move(node));
    }
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode overBudget;
    overBudget.instanceId = "rootMissing";
    overBudget.typeId = "org.example.unavailable";
    overBudget.preservedComponent = component;
    document.graph.nodes.push_back(std::move(overBudget));
    talas::project::reconcileProjectPresentation(document);

    const auto parsed = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(
        !parsed.ok()
            && parsed.errors.back().message.find("count")
                != std::string::npos,
        "the parser should report the shared preserved-component count budget");
    require(
        parsed.document.soundDefinitions.size() == 1U
            && parsed.document.soundDefinitions.front().graph.nodes.size()
                == talas::project::maximumProjectPreservedComponents
            && parsed.document.graph.nodes.size() == 1U
            && !parsed.document.graph.nodes.back()
                    .preservedComponent.has_value(),
        "one parser budget must span nested Sound definitions and reject the first over-budget root capsule before retention");
}
}

int main()
{
    testOpaqueStateUsesCanonicalBase64();
    testPreservedComponentValidationIsBoundedAndStructural();
    testAvailabilityRequiresAnExactTrustedDescriptor();
    testStructuralValidationPreservesButExecutionRejectsMissingNodes();
    testNestedSoundPreservationRetainsQualifiedScope();
    testEditingValidationAvoidsSyntheticTypeIdentityCollisions();
    testParserRejectsPreservedComponentCountBeforeRetainingTheNextCapsule();
    return 0;
}
