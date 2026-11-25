#include "talas/core/BuiltinNodes.h"
#include "talas/core/NodeCatalogJson.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(std::string_view haystack, std::string_view needle, const std::string& message)
{
    require(haystack.find(needle) != std::string_view::npos, message);
}

void testBuiltinCatalogSerializesDeterministicJson()
{
    const auto json = talas::core::serializeNodeCatalog(talas::core::makeBuiltinNodeRegistry());

    require(json.starts_with("{\n  \"nodes\": [\n"), "catalog JSON should start with nodes array");
    require(json.ends_with("}\n"), "catalog JSON should end with newline");
    requireContains(json, R"json("typeId": "talas.audioInput")json", "catalog should include audio input node");
    requireContains(json, R"json("displayName": "Audio Input")json", "catalog should include display name");
    requireContains(json, R"json("category": "IO")json", "catalog should include category");
    requireContains(
        json,
        R"json("vendor": "Talas")json",
        "catalog should expose explicit descriptor vendor provenance");
    requireContains(
        json,
        R"json("origin": "builtIn")json",
        "catalog should expose explicit descriptor origin provenance");
    requireContains(
        json,
        R"json("supportLevel": "functionalSubset")json",
        "catalog should expose runtime maturity");
    requireContains(
        json,
        R"json("supportSummary": "Subset:)json",
        "catalog should expose a concise support summary");
    requireContains(json, R"json("capabilities": {
        "offline": "subset",
        "offlineBinding": "legacyAdapter",
        "preparedRealtime": "subset",
        "preparedRealtimeBinding": "legacyAdapter")json",
        "catalog should expose orthogonal execution axes");
    requireContains(json, R"json("limitations": [
          "offline-functional-subset",
          "prepared-realtime-subset"
        ])json",
        "catalog should expose stable limitation IDs");
    requireContains(
        json,
        R"json("arbitrary-script-runtime-unsupported")json",
        "catalog should expose the closed script-runtime limitation");
    requireContains(json, R"json("outputs": [
        {
          "id": "audioOut",
          "type": "audio",
          "required": false,
          "rate": "sample",
          "audioLayout": "mono",
          "edge": "continuous",
          "timelineBearing": true,
          "spectralFormat": "notApplicable"
        }
      ])json", "catalog should include typed output ports");
    requireContains(json, R"json("id": "channel1",
          "kind": "boolean",
          "domain": "scalar",
          "defaultBoolean": true)json", "catalog should include boolean defaults");

    const auto audioInputPosition = json.find(R"json("typeId": "talas.audioInput")json");
    const auto oscillatorPosition = json.find(R"json("typeId": "talas.oscillator")json");
    require(audioInputPosition != std::string::npos, "catalog should contain audio input");
    require(oscillatorPosition != std::string::npos, "catalog should contain oscillator");
    require(audioInputPosition < oscillatorPosition, "catalog should be sorted by type id");
}

void testCatalogSerializesParameterKinds()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.test",
        "Test",
        "Testing",
        {},
        {},
        {
            {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
            talas::core::choiceParameter("mode", "a", {"a", "b"}),
            talas::core::booleanParameter("enabled", true),
            talas::core::resourceParameter("sample", "Samples/A"),
            talas::core::textParameter("script", "run"),
        },
    });

    const auto json = talas::core::serializeNodeCatalog(registry);

    requireContains(json, R"json("kind": "expression")json", "catalog should serialize expression parameters");
    requireContains(json, R"json("domain": "hertz")json", "catalog should serialize parameter domain");
    requireContains(json, R"json("defaultExpression": "440 hz")json", "catalog should serialize expression defaults");
    requireContains(json, R"json("kind": "choice")json", "catalog should serialize choice parameters");
    requireContains(json, R"json("defaultChoice": "a")json", "catalog should serialize default choice");
    requireContains(json, R"json("choices": [
            "a",
            "b"
          ])json", "catalog should serialize choices");
    requireContains(json, R"json("kind": "boolean")json", "catalog should serialize boolean parameters");
    requireContains(json, R"json("defaultBoolean": true)json", "catalog should serialize boolean defaults");
    requireContains(json, R"json("kind": "resource")json", "catalog should serialize resource parameters");
    requireContains(json, R"json("defaultResource": "Samples/A")json", "catalog should serialize resource defaults");
    requireContains(json, R"json("kind": "text")json", "catalog should serialize text parameters");
    requireContains(json, R"json("defaultText": "run")json", "catalog should serialize text defaults");
    requireContains(json, R"json("supportLevel": "utility")json", "catalog should serialize default utility maturity");
    requireContains(
        json,
        R"json("supportSummary": "Unclassified utility node.")json",
        "catalog should serialize the default support summary");
}

void testCatalogSerializesM3ExecutionContracts()
{
    talas::core::NodeTypeDescriptor descriptor{
        "talas.contract",
        "Contract",
        "Testing",
        {{"frames", talas::core::PortType::ComplexSpectrum, true}},
        {{"audio", talas::core::PortType::Audio, false}},
        {
            {"length", "64", talas::expr::ValueDomain::Samples},
            talas::core::resourceParameter("source", {}),
        },
    };
    descriptor.capabilities.latency = {
        talas::core::LatencyKind::PreparationTime,
        0,
        "length",
        talas::core::PreparedLatencyRule::ParameterFramesMinusOne,
    };
    descriptor.capabilities.resourceAccess =
        talas::core::ResourceAccessMode::StreamRead;
    descriptor.capabilities.fanInLatencyPolicy =
        talas::core::FanInLatencyPolicy::Align;
    descriptor.capabilities.outputBusLayouts = {
        talas::core::AudioBusLayoutKind::Discrete4,
    };
    talas::core::NodeRegistry registry;
    registry.addNodeType(std::move(descriptor));

    const auto json = talas::core::serializeNodeCatalog(registry);

    requireContains(json, R"json("type": "complexSpectrum")json",
                    "catalog should distinguish complex spectral ports");
    requireContains(json, R"json("rate": "frame")json",
                    "catalog should expose spectral frame rate");
    requireContains(json, R"json("audioLayout": "mono")json",
                    "catalog should expose per-port channel layout");
    requireContains(json, R"json("timelineBearing": true)json",
                    "catalog should expose timeline-bearing ports");
    requireContains(json, R"json("spectralFormat": "complexSpectrumV1")json",
                    "catalog should expose complex-frame format");
    requireContains(json, R"json("kind": "preparationTime")json",
                    "catalog should expose preparation-time latency");
    requireContains(json, R"json("parameterId": "length")json",
                    "catalog should expose latency parameter authority");
    requireContains(json, R"json("preparedRule": "parameterFramesMinusOne")json",
                    "catalog should expose the prepared latency rule");
    requireContains(json, R"json("resourceAccess": "streamRead")json",
                    "catalog should expose stream ownership intent");
    requireContains(json, R"json("fanInLatencyPolicy": "align")json",
                    "catalog should expose fan-in policy");
    requireContains(json, R"json("outputBusLayouts": [
          "discrete4"
        ])json",
                    "catalog should expose supported terminal layouts");
}

void testCatalogEscapesStrings()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.quote",
        "Quote \"Node\"",
        "Cat\\Line\nTwo",
        {},
        {},
        {
            {"expr", "1\n2\\3", talas::expr::ValueDomain::Scalar},
        },
    });

    const auto json = talas::core::serializeNodeCatalog(registry);

    requireContains(json, R"json("displayName": "Quote \"Node\"")json", "catalog should escape quotes");
    requireContains(json, R"json("category": "Cat\\Line\nTwo")json", "catalog should escape backslashes and newlines");
    requireContains(json, R"json("defaultExpression": "1\n2\\3")json", "catalog should escape parameter default strings");
}

void testCheckedCatalogSnapshotMatchesRegistryExactly()
{
    const auto actual = talas::core::serializeNodeCatalog(
        talas::core::makeBuiltinNodeRegistry());
    std::ifstream input(
        std::string{TALAS_SOURCE_DIR}
            + "/docs/coverage/builtin-node-catalog-v2.json",
        std::ios::binary);
    require(input.good(), "checked catalog snapshot should be readable");
    const std::string expected{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    require(
        actual == expected,
        "checked catalog snapshot should match the NodeRegistry projection exactly");
}
}

int main()
{
    testBuiltinCatalogSerializesDeterministicJson();
    testCatalogSerializesParameterKinds();
    testCatalogSerializesM3ExecutionContracts();
    testCatalogEscapesStrings();
    testCheckedCatalogSnapshotMatchesRegistryExactly();
    return 0;
}
