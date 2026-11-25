#include "talas/core/Graph.h"
#include "talas/core/NodeRegistry.h"

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool hasValidationError(const talas::core::ValidationResult& result, talas::core::ValidationCode code)
{
    for (const auto& error : result.errors)
    {
        if (error.code == code)
            return true;
    }

    return false;
}

void testRegistryFindsNodeType()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {},
        {{"audioOut", talas::core::PortType::Audio, true}},
        {{"frequency", "440 hz"}}
    });

    const auto* descriptor = registry.findNodeType("talas.oscillator");
    require(descriptor != nullptr, "registry should find oscillator node type");
    require(descriptor->displayName == "Oscillator", "display name should be preserved");
    require(descriptor->outputs.size() == 1, "oscillator should expose one output");
}

void testRegistryRejectsDuplicates()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({"talas.constant", "Constant", "Math", {}, {{"value", talas::core::PortType::Control, true}}, {}});

    bool rejected = false;
    try
    {
        registry.addNodeType({"talas.constant", "Constant", "Math", {}, {{"value", talas::core::PortType::Control, true}}, {}});
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    require(rejected, "registry should reject duplicate node type ids");
}

void testRegistryRequiresBoundedExplicitVendorProvenance()
{
    talas::core::NodeTypeDescriptor descriptor{
        "talas.external",
        "External",
        "Test",
        {},
        {},
        {},
    };
    descriptor.provenance.vendor.clear();
    descriptor.provenance.origin = talas::core::NodeOrigin::External;
    bool rejected = false;
    try
    {
        talas::core::NodeRegistry registry;
        registry.addNodeType(std::move(descriptor));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "registry should reject empty external vendor provenance instead of inferring from type ID");

    talas::core::NodeTypeDescriptor unknownOrigin{
        "talas.unknown-origin",
        "Unknown origin",
        "Test",
        {},
        {},
        {},
    };
    unknownOrigin.provenance.vendor = "External Vendor";
    unknownOrigin.provenance.origin =
        static_cast<talas::core::NodeOrigin>(255);
    rejected = false;
    try
    {
        talas::core::NodeRegistry registry;
        registry.addNodeType(std::move(unknownOrigin));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "registry should reject an unknown provenance origin instead of serializing it as a known origin");
}

void requireRegistryRejectsParameter(talas::core::ParameterDescriptor parameter, const std::string& message)
{
    talas::core::NodeRegistry registry;
    bool rejected = false;
    try
    {
        registry.addNodeType({
            "talas.invalid",
            "Invalid",
            "Test",
            {},
            {},
            {std::move(parameter)}
        });
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    require(rejected, message);
}

void testRegistryRejectsMalformedParameterDescriptors()
{
    requireRegistryRejectsParameter(
        {"gain", "1.0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Expression, "power", {}},
        "expression parameter should reject default choice");
    requireRegistryRejectsParameter(
        {"gain", "1.0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Expression, {}, {"power"}},
        "expression parameter should reject choice list");
    requireRegistryRejectsParameter(
        {"mode", "0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {"power", "linear"}},
        "choice parameter should reject default expression");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, {}, {"power", "linear"}},
        "choice parameter should reject empty default choice");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {}},
        "choice parameter should reject empty choice list");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "equalPower", {"power", "linear"}},
        "choice parameter should reject default choice outside choice list");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {"power", "power"}},
        "choice parameter should reject duplicate choices");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {"power", ""}},
        "choice parameter should reject empty choice values");
    requireRegistryRejectsParameter(
        {"loop", "1.0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Boolean, {}, {}, false},
        "boolean parameter should reject default expression");
    requireRegistryRejectsParameter(
        {"loop", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Boolean, "on", {}, false},
        "boolean parameter should reject default choice");
    requireRegistryRejectsParameter(
        {"loop", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Boolean, {}, {"off", "on"}, false},
        "boolean parameter should reject choice list");
    requireRegistryRejectsParameter(
        {"gain", "1.0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Expression, {}, {}, true},
        "expression parameter should reject boolean default");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {"power", "linear"}, true},
        "choice parameter should reject boolean default");
    requireRegistryRejectsParameter(
        {"gain", "1.0", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Expression, {}, {}, false},
        "expression parameter should reject explicit false boolean default");
    requireRegistryRejectsParameter(
        {"mode", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Choice, "power", {"power", "linear"}, false},
        "choice parameter should reject explicit false boolean default");
    requireRegistryRejectsParameter(
        {"file", "sample.wav", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Resource},
        "resource parameter should reject default expression");
    requireRegistryRejectsParameter(
        {"file", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Resource, "sample.wav"},
        "resource parameter should reject default choice");
    requireRegistryRejectsParameter(
        {"file", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Resource, {}, {"sample.wav"}},
        "resource parameter should reject choice list");
    requireRegistryRejectsParameter(
        {"file", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Resource, {}, {}, true},
        "resource parameter should reject boolean default");
    requireRegistryRejectsParameter(
        {"script", "1 + 1", talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Text},
        "text parameter should reject default expression");
    requireRegistryRejectsParameter(
        {"script", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Text, "linear"},
        "text parameter should reject default choice");
    requireRegistryRejectsParameter(
        {"script", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Text, {}, {"linear"}},
        "text parameter should reject choice list");
    requireRegistryRejectsParameter(
        {"script", {}, talas::expr::ValueDomain::Scalar, talas::core::ParameterKind::Text, {}, {}, false},
        "text parameter should reject explicit false boolean default");
}

void testBooleanParameterHelperCreatesDescriptor()
{
    const auto parameter = talas::core::booleanParameter("loop", true);

    require(parameter.id == "loop", "boolean helper should preserve id");
    require(parameter.kind == talas::core::ParameterKind::Boolean, "boolean helper should set Boolean kind");
    require(parameter.defaultBoolean.has_value(), "boolean helper should set boolean default");
    require(*parameter.defaultBoolean, "boolean helper should preserve default value");
    require(parameter.defaultExpression.empty(), "boolean helper should not set default expression");
    require(parameter.defaultChoice.empty(), "boolean helper should not set default choice");
    require(parameter.choices.empty(), "boolean helper should not set choices");
}

void testResourceAndTextParameterHelpersCreateDescriptors()
{
    const auto resource = talas::core::resourceParameter("sample", "kick.wav");

    require(resource.id == "sample", "resource helper should preserve id");
    require(resource.kind == talas::core::ParameterKind::Resource, "resource helper should set Resource kind");
    require(resource.defaultResource == "kick.wav", "resource helper should preserve default resource");
    require(resource.defaultExpression.empty(), "resource helper should not set default expression");
    require(resource.defaultChoice.empty(), "resource helper should not set default choice");
    require(resource.choices.empty(), "resource helper should not set choices");
    require(!resource.defaultBoolean.has_value(), "resource helper should not set boolean default");

    const auto text = talas::core::textParameter("script", "1 + 1");

    require(text.id == "script", "text helper should preserve id");
    require(text.kind == talas::core::ParameterKind::Text, "text helper should set Text kind");
    require(text.defaultText == "1 + 1", "text helper should preserve default text");
    require(text.defaultExpression.empty(), "text helper should not set default expression");
    require(text.defaultChoice.empty(), "text helper should not set default choice");
    require(text.choices.empty(), "text helper should not set choices");
    require(!text.defaultBoolean.has_value(), "text helper should not set boolean default");
}

void testRegistryReturnsNodeTypesSortedByTypeId()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({"talas.zeta", "Zeta", "Test", {}, {}, {}});
    registry.addNodeType({"talas.alpha", "Alpha", "Test", {}, {}, {}});
    registry.addNodeType({"talas.middle", "Middle", "Test", {}, {}, {}});

    const auto descriptors = registry.nodeTypes();
    require(descriptors.size() == 3, "registry should return every node type");
    require(descriptors[0].typeId == "talas.alpha", "first node type should be sorted by type id");
    require(descriptors[1].typeId == "talas.middle", "second node type should be sorted by type id");
    require(descriptors[2].typeId == "talas.zeta", "third node type should be sorted by type id");
}

talas::core::NodeRegistry makeRegistry()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Sources",
        {},
        {{"audioOut", talas::core::PortType::Audio, true}},
        {{"frequency", "440 hz"}}
    });
    registry.addNodeType({
        "talas.vca",
        "VCA",
        "Mixing",
        {
            {"audioIn", talas::core::PortType::Audio, true},
            {"gain", talas::core::PortType::Control, false}
        },
        {{"audioOut", talas::core::PortType::Audio, true}},
        {}
    });
    registry.addNodeType({
        "talas.constant",
        "Constant",
        "Math",
        {},
        {{"value", talas::core::PortType::Control, true}},
        {{"value", "1.0"}}
    });
    registry.addNodeType({
        "talas.audioOutput",
        "AudioOutput",
        "IO",
        {{"audioIn", talas::core::PortType::Audio, true}},
        {},
        {}
    });
    return registry;
}

void testValidGraphPasses()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"amp1", "talas.vca"});
    graph.addNode({"out1", "talas.audioOutput"});
    graph.connect({"osc1", "audioOut", "amp1", "audioIn"});
    graph.connect({"amp1", "audioOut", "out1", "audioIn"});

    const auto result = graph.validate(registry);
    require(result.ok(), "valid oscillator-vca-output graph should pass validation");
}

void testUnknownNodeTypeFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"bad1", "talas.missing"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "unknown node type should fail validation");
    require(result.errors[0].code == talas::core::ValidationCode::UnknownNodeType, "expected unknown node type error");
}

void testMissingRequiredInputFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"amp1", "talas.vca"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "missing required audio input should fail validation");
    require(result.errors[0].code == talas::core::ValidationCode::MissingRequiredInput, "expected missing input error");
}

// Any signal may feed any input: compiling adapts a control
// output to an audio input and the reverse (ADR-0016).
void testControlOutputFeedsAudioInput()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"constant1", "talas.constant"});
    graph.addNode({"out1", "talas.audioOutput"});
    graph.connect({"constant1", "value", "out1", "audioIn"});

    const auto result = graph.validate(registry);
    require(std::ranges::none_of(result.errors, [](const auto& error) {
                return error.code == talas::core::ValidationCode::PortTypeMismatch;
            }),
        "a control output should feed an audio input");
}

void testPartialAndComplexSpectrumPortsCannotConnect()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.complexSource",
        "Complex Source",
        "Testing",
        {},
        {{"frames", talas::core::PortType::ComplexSpectrum, false}},
        {},
    });
    registry.addNodeType({
        "talas.partialSink",
        "Partial Sink",
        "Testing",
        {{"partials", talas::core::PortType::Spectrum, true}},
        {},
        {},
    });
    talas::core::Graph graph;
    graph.addNode({"source", "talas.complexSource"});
    graph.addNode({"sink", "talas.partialSink"});
    graph.connect({"source", "frames", "sink", "partials"});

    const auto validation = graph.validate(registry);
    require(
        std::ranges::any_of(
            validation.errors,
            [](const auto& error) {
                return error.code
                    == talas::core::ValidationCode::PortTypeMismatch;
            }),
        "partial and complex spectral frame ports must not connect implicitly");
}

void testNativePartialSpectrumFeedsLegacyInputsButNotTheReverse()
{
    const talas::core::PortSignalContract nativePartialSignal{
        talas::core::PortSignalRate::Frame,
        talas::core::AudioBusLayoutKind::Mono,
        talas::core::PortEdgeSemantics::Continuous,
        talas::core::SpectralFrameFormat::PartialSpectrumV1,
        true,
    };
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.nativePartialSource",
        "Native Partial Source",
        "Test",
        {},
        {{"spectrumOut", talas::core::PortType::Spectrum, false,
          nativePartialSignal}},
        {},
    });
    registry.addNodeType({
        "talas.legacyPartialConsumer",
        "Legacy Partial Consumer",
        "Test",
        {{"spectrumIn", talas::core::PortType::Spectrum, true}},
        {},
        {},
    });
    registry.addNodeType({
        "talas.partialSpectrumLegacyHoldV1",
        "Partial Spectrum Legacy Hold",
        "Test",
        {{"spectrumIn", talas::core::PortType::Spectrum, true,
          nativePartialSignal}},
        {{"spectrumOut", talas::core::PortType::Spectrum, false}},
        {},
    });

    talas::core::Graph graph;
    graph.addNode({"source", "talas.nativePartialSource"});
    graph.addNode({"consumer", "talas.legacyPartialConsumer"});
    graph.connect({"source", "spectrumOut", "consumer", "spectrumIn"});

    // A timestamped spectrum feeds a legacy Spectrum input directly;
    // compiling inserts the hold (ADR-0016, amended 2026-10-02).
    require(graph.validate(registry).ok(),
            "a native partial spectrum should connect to a legacy Spectrum input");

    talas::core::Graph adapted;
    adapted.addNode({"source", "talas.nativePartialSource"});
    adapted.addNode({"hold", "talas.partialSpectrumLegacyHoldV1"});
    adapted.addNode({"consumer", "talas.legacyPartialConsumer"});
    adapted.connect({"source", "spectrumOut", "hold", "spectrumIn"});
    adapted.connect({"hold", "spectrumOut", "consumer", "spectrumIn"});
    require(adapted.validate(registry).ok(),
            "the explicit hold should be the valid native-to-legacy boundary");

    talas::core::Graph reverse;
    reverse.addNode({"hold", "talas.partialSpectrumLegacyHoldV1"});
    reverse.addNode({"native", "talas.nativePartialSource"});
    reverse.connect({"hold", "spectrumOut", "hold", "spectrumIn"});
    const auto reverseResult = reverse.validate(registry);
    require(hasValidationError(
                reverseResult,
                talas::core::ValidationCode::PortTypeMismatch),
            "legacy-to-native partial wiring should also fail closed");
}

void testLongChainsValidateWithoutDeepRecursion()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    constexpr std::size_t length = 200000;
    graph.addNode({"osc", "talas.oscillator"});
    std::string previous = "osc";
    for (std::size_t index = 0; index < length; ++index)
    {
        auto id = "amp" + std::to_string(index);
        graph.addNode({id, "talas.vca"});
        graph.connect({previous, "audioOut", id, "audioIn"});
        previous = std::move(id);
    }
    graph.addNode({"out", "talas.audioOutput"});
    graph.connect({previous, "audioOut", "out", "audioIn"});
    require(graph.validate(registry).ok(),
        "a very long processing chain should validate without exhausting the stack");
}

void testDirectCycleFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"amp1", "talas.vca"});
    graph.addNode({"amp2", "talas.vca"});
    graph.connect({"amp1", "audioOut", "amp2", "audioIn"});
    graph.connect({"amp2", "audioOut", "amp1", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "direct cycle should fail validation");

    bool sawCycle = false;
    for (const auto& error : result.errors)
        sawCycle = sawCycle || error.code == talas::core::ValidationCode::CycleDetected;
    require(sawCycle, "expected cycle detected error");
}

void testDuplicateNodeInstanceFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"amp1", "talas.vca"});
    graph.addNode({"amp1", "talas.vca"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "duplicate node instance ids should fail validation");
    require(hasValidationError(result, talas::core::ValidationCode::DuplicateNodeInstance), "expected duplicate node instance error");
}

void testUnknownOutputPortFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"amp1", "talas.vca"});
    graph.connect({"osc1", "missingOut", "amp1", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "unknown output port should fail validation");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownPort), "expected unknown output port error");
}

void testUnknownInputPortFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"amp1", "talas.vca"});
    graph.connect({"osc1", "audioOut", "amp1", "missingIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "unknown input port should fail validation");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownPort), "expected unknown input port error");
}

void testStaleSourceNodeReferenceFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"amp1", "talas.vca"});
    graph.connect({"deletedOsc", "audioOut", "amp1", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "stale source node reference should fail validation");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownNodeInstance), "expected unknown source node instance error");
}

void testStaleTargetNodeReferenceFails()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.connect({"osc1", "audioOut", "deletedAmp", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "stale target node reference should fail validation");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownNodeInstance), "expected unknown target node instance error");
}

void testStaleConnectionDoesNotSatisfyRequiredInput()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"amp1", "talas.vca"});
    graph.connect({"deletedOsc", "audioOut", "amp1", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "stale connection should not satisfy required input");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownNodeInstance), "expected unknown node instance error");
    require(hasValidationError(result, talas::core::ValidationCode::MissingRequiredInput), "expected missing required input error");
}

void testInvalidPortConnectionDoesNotSatisfyRequiredInput()
{
    const auto registry = makeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"amp1", "talas.vca"});
    graph.connect({"osc1", "missingOut", "amp1", "audioIn"});

    const auto result = graph.validate(registry);
    require(!result.ok(), "invalid port connection should not satisfy required input");
    require(hasValidationError(result, talas::core::ValidationCode::UnknownPort), "expected unknown port error");
    require(hasValidationError(result, talas::core::ValidationCode::MissingRequiredInput), "expected missing required input error");
}
}

int main()
{
    testRegistryFindsNodeType();
    testRegistryRejectsDuplicates();
    testRegistryRequiresBoundedExplicitVendorProvenance();
    testRegistryRejectsMalformedParameterDescriptors();
    testBooleanParameterHelperCreatesDescriptor();
    testResourceAndTextParameterHelpersCreateDescriptors();
    testRegistryReturnsNodeTypesSortedByTypeId();
    testValidGraphPasses();
    testUnknownNodeTypeFails();
    testMissingRequiredInputFails();
    testControlOutputFeedsAudioInput();
    testPartialAndComplexSpectrumPortsCannotConnect();
    testNativePartialSpectrumFeedsLegacyInputsButNotTheReverse();
    testDirectCycleFails();
    testLongChainsValidateWithoutDeepRecursion();
    testDuplicateNodeInstanceFails();
    testUnknownOutputPortFails();
    testUnknownInputPortFails();
    testStaleSourceNodeReferenceFails();
    testStaleTargetNodeReferenceFails();
    testStaleConnectionDoesNotSatisfyRequiredInput();
    testInvalidPortConnectionDoesNotSatisfyRequiredInput();
    return 0;
}
