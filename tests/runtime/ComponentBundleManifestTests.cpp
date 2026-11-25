#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace
{
namespace bundle = talas::runtime::component_bundle;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string validManifestJson()
{
    return R"json({
  "format": "talas.component-bundle",
  "version": 1,
  "bundleId": "org.example.gain",
  "semanticVersion": "1.2.3-beta.1+build.7",
  "engine": {
    "abiMinimum": 1,
    "abiMaximum": 2,
    "apiMinimum": 3,
    "apiMaximum": 4
  },
  "roles": ["realtime-processor", "declarative-ui-provider"],
  "permissions": ["realtime-audio", "declarative-ui"],
  "entry": {
    "path": "Payload/component.bin",
    "bytes": 3,
    "sha256": "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
  },
  "files": [
    {
      "path": "Assets/license.txt",
      "purpose": "license",
      "bytes": 7,
      "sha256": "101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"
    },
    {
      "path": "Assets/ui.json",
      "purpose": "declarative-ui",
      "bytes": 2,
      "sha256": "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
    }
  ],
  "nodes": [
    {
      "typeId": "org.example.gain.mono",
      "stateSchemaVersion": 2,
      "inputs": [
        {
          "id": "audio-in",
          "type": "audio",
          "required": true,
          "rate": "sample",
          "audioLayout": "mono",
          "edge": "continuous",
          "spectralFormat": "not-applicable",
          "timelineBearing": true
        }
      ],
      "outputs": [
        {
          "id": "audio-out",
          "type": "audio",
          "required": false,
          "rate": "sample",
          "audioLayout": "mono",
          "edge": "continuous",
          "spectralFormat": "not-applicable",
          "timelineBearing": true
        }
      ],
      "parameters": [
        {
          "id": "gain",
          "kind": "expression",
          "domain": "scalar",
          "choices": []
        },
        {
          "id": "mode",
          "kind": "choice",
          "domain": "scalar",
          "choices": ["clean", "warm"]
        }
      ]
    }
  ],
  "dependencies": [
    {
      "bundleId": "org.example.foundation",
      "minimumVersion": "1.0.0",
      "maximumExclusiveVersion": "2.0.0",
      "optional": false
    }
  ],
  "state": {
    "currentSchemaVersion": 2,
    "migrations": [
      {"from": 1, "to": 2, "mode": "host-mediated"}
    ]
  },
  "provenance": {
    "publisher": "Example Audio",
    "licenseExpression": "Apache-2.0",
    "notices": ["Copyright Example"]
  },
  "declarativeUiPath": "Assets/ui.json"
})json";
}

std::string replaced(
    std::string source,
    const std::string& before,
    const std::string& after)
{
    const auto position = source.find(before);
    require(position != std::string::npos, "test replacement source should exist");
    source.replace(position, before.size(), after);
    return source;
}

bool hasCode(
    const bundle::ComponentBundleManifestParseResult& result,
    bundle::ComponentBundleManifestErrorCode code)
{
    return std::ranges::any_of(
        result.errors,
        [code](const auto& error) { return error.code == code; });
}

void testCompleteManifestParsesAndCanonicalizes()
{
    const auto parsed = bundle::parseComponentBundleManifest(validManifestJson());
    require(parsed.ok(), "complete bounded v1 manifest should parse");
    require(
        parsed.manifest->bundleId == "org.example.gain"
            && parsed.manifest->nodes.size() == 1
            && parsed.manifest->files.size() == 2
            && parsed.manifest->declaredRoles
                == (talas::runtime::componentRole(
                        talas::runtime::ComponentExecutionRole::RealtimeProcessor)
                    | talas::runtime::componentRole(
                        talas::runtime::ComponentExecutionRole::DeclarativeUiProvider)),
        "parsed manifest should retain the closed identity and schema model");

    const auto canonical =
        bundle::writeCanonicalComponentBundleManifest(*parsed.manifest);
    require(canonical.ok(), "accepted manifest should have a canonical encoding");
    require(
        canonical.canonicalJson.find('\n') == std::string::npos
            && canonical.canonicalJson.starts_with(
                R"json({"format":"talas.component-bundle","version":1,"bundleId":"org.example.gain")json"),
        "canonical encoding should use the fixed root order without whitespace");

    const auto roundTrip =
        bundle::parseComponentBundleManifest(canonical.canonicalJson);
    require(
        roundTrip.ok() && *roundTrip.manifest == *parsed.manifest,
        "canonical parse/write/parse should preserve the exact typed model");
    const auto repeated =
        bundle::writeCanonicalComponentBundleManifest(*roundTrip.manifest);
    require(
        repeated.ok() && repeated.canonicalJson == canonical.canonicalJson,
        "canonical encoding should be byte-identical after repetition");

    auto permuted = *parsed.manifest;
    std::ranges::reverse(permuted.files);
    std::ranges::reverse(permuted.provenance.notices);
    const auto normalizedPermutation =
        bundle::writeCanonicalComponentBundleManifest(permuted);
    require(
        normalizedPermutation.ok()
            && normalizedPermutation.canonicalJson == canonical.canonicalJson,
        "unordered manifest sets should normalize to one canonical order");
}

void testUnknownFieldsAndNoncanonicalIntegersReject()
{
    const auto unknown = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json("declarativeUiPath": "Assets/ui.json")json",
        R"json("unexpected": true, "declarativeUiPath": "Assets/ui.json")json"));
    require(
        !unknown.ok() && hasCode(unknown, bundle::ComponentBundleManifestErrorCode::UnknownField),
        "unknown schema members should reject instead of being ignored");

    const auto fractionalVersion = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json("version": 1)json",
        R"json("version": 1.0)json"));
    require(
        !fractionalVersion.ok()
            && hasCode(fractionalVersion, bundle::ComponentBundleManifestErrorCode::InvalidInteger),
        "manifest integers should require canonical decimal tokens");
}

void testIdentityVersionRoleAndPermissionRulesRejectAmbiguity()
{
    const auto upperBundle = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "org.example.gain", "Org.example.gain"));
    require(
        !upperBundle.ok()
            && hasCode(upperBundle, bundle::ComponentBundleManifestErrorCode::InvalidIdentity),
        "bundle identity should use one lowercase reverse-DNS grammar");

    const auto leadingZeroVersion = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "1.2.3-beta.1+build.7", "01.2.3"));
    require(
        !leadingZeroVersion.ok()
            && hasCode(leadingZeroVersion, bundle::ComponentBundleManifestErrorCode::InvalidSemanticVersion),
        "semantic versions should reject ambiguous leading zeroes");

    const auto nonAsciiVersion = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "1.2.3-beta.1+build.7", "1.2.é"));
    require(
        !nonAsciiVersion.ok()
            && hasCode(nonAsciiVersion, bundle::ComponentBundleManifestErrorCode::InvalidSemanticVersion),
        "semantic-version identity should remain ASCII even when JSON is valid UTF-8");

    const auto mismatchedPermissions = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json(["realtime-audio", "declarative-ui"])json",
        R"json(["realtime-audio"])json"));
    require(
        !mismatchedPermissions.ok()
            && hasCode(mismatchedPermissions, bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration),
        "declared permissions should equal the exact role-derived permission set");

    const auto duplicateRole = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json(["realtime-processor", "declarative-ui-provider"])json",
        R"json(["realtime-processor", "realtime-processor"])json"));
    require(
        !duplicateRole.ok()
            && hasCode(duplicateRole, bundle::ComponentBundleManifestErrorCode::DuplicateIdentity),
        "duplicate role declarations should reject");

    const auto unknownPortType = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json("type": "audio")json",
        R"json("type": "mystery")json"));
    require(
        !unknownPortType.ok()
            && hasCode(unknownPortType, bundle::ComponentBundleManifestErrorCode::InvalidEnum),
        "unknown closed node-schema enums should reject");
}

void testPayloadPathsDigestsAndUiReferenceAreClosed()
{
    const auto traversal = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "Payload/component.bin", "Payload/../component.bin"));
    require(
        !traversal.ok()
            && hasCode(traversal, bundle::ComponentBundleManifestErrorCode::InvalidPath),
        "dot-dot payload paths should reject");

    const auto uppercaseDigest = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
        "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F"));
    require(
        !uppercaseDigest.ok()
            && hasCode(uppercaseDigest, bundle::ComponentBundleManifestErrorCode::InvalidDigest),
        "payload digests should accept lowercase canonical SHA-256 only");

    const auto missingUiAsset = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json("declarativeUiPath": "Assets/ui.json")json",
        R"json("declarativeUiPath": "Assets/missing.json")json"));
    require(
        !missingUiAsset.ok()
            && hasCode(missingUiAsset, bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration),
        "declarative UI should reference one declared UI-purpose asset");

    const auto reservedManifestPath = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "Assets/license.txt", "manifest.json"));
    require(
        !reservedManifestPath.ok()
            && hasCode(reservedManifestPath, bundle::ComponentBundleManifestErrorCode::InvalidPath),
        "declared payloads should not alias the root manifest entry");

    const auto reservedSignaturePath = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "Assets/license.txt", "SIGNATURE.CMS"));
    require(
        !reservedSignaturePath.ok()
            && hasCode(
                reservedSignaturePath,
                bundle::ComponentBundleManifestErrorCode::InvalidPath),
        "declared payloads must not alias the detached signature envelope");
}

void testNodeSchemaDependenciesAndMigrationsRejectDuplicates()
{
    const auto foreignType = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "org.example.gain.mono", "org.other.component.mono"));
    require(
        !foreignType.ok()
            && hasCode(foreignType, bundle::ComponentBundleManifestErrorCode::InvalidIdentity),
        "node type identities should remain below the owning bundle namespace");

    const auto duplicatePort = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "audio-out", "audio-in"));
    require(
        !duplicatePort.ok()
            && hasCode(duplicatePort, bundle::ComponentBundleManifestErrorCode::DuplicateIdentity),
        "input and output IDs should be unique across one node interface");

    const auto selfDependency = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(), "org.example.foundation", "org.example.gain"));
    require(
        !selfDependency.ok()
            && hasCode(selfDependency, bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration),
        "self-dependencies should reject during manifest validation");

    const auto emptyDependencyRange = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json("maximumExclusiveVersion": "2.0.0")json",
        R"json("maximumExclusiveVersion": "1.0.0")json"));
    require(
        !emptyDependencyRange.ok()
            && hasCode(
                emptyDependencyRange,
                bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration),
        "dependency minimum must be strictly below its exclusive maximum");

    const auto backwardMigration = bundle::parseComponentBundleManifest(replaced(
        validManifestJson(),
        R"json({"from": 1, "to": 2, "mode": "host-mediated"})json",
        R"json({"from": 2, "to": 1, "mode": "host-mediated"})json"));
    require(
        !backwardMigration.ok()
            && hasCode(backwardMigration, bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration),
        "migration metadata should be strictly forward-only");
}

void testWriterRejectsCallerConstructedInvalidModels()
{
    const auto parsed = bundle::parseComponentBundleManifest(validManifestJson());
    require(parsed.ok(), "writer validation fixture should parse first");
    auto invalid = *parsed.manifest;
    invalid.entry.path = "../escape";

    const auto written = bundle::writeCanonicalComponentBundleManifest(invalid);
    require(
        !written.ok() && written.canonicalJson.empty()
            && std::ranges::any_of(
                written.errors,
                [](const auto& error) {
                    return error.code
                        == bundle::ComponentBundleManifestErrorCode::InvalidPath;
                }),
        "canonical writer should reject rather than serialize an invalid model");

    auto caseCollision = *parsed.manifest;
    caseCollision.files[1].path = "assets/LICENSE.txt";
    const auto collided =
        bundle::writeCanonicalComponentBundleManifest(caseCollision);
    require(
        !collided.ok()
            && std::ranges::any_of(
                collided.errors,
                [](const auto& error) {
                    return error.code
                        == bundle::ComponentBundleManifestErrorCode::DuplicateIdentity;
                }),
        "writer validation should reject case-folded payload collisions");

    auto contradictoryPort = *parsed.manifest;
    contradictoryPort.nodes.front().inputs.front().signal.rate =
        talas::core::PortSignalRate::Block;
    const auto contradictory =
        bundle::writeCanonicalComponentBundleManifest(contradictoryPort);
    require(
        !contradictory.ok()
            && std::ranges::any_of(
                contradictory.errors,
                [](const auto& error) {
                    return error.code
                        == bundle::ComponentBundleManifestErrorCode::ContradictoryDeclaration;
                }),
        "writer validation should reject port type/signal contradictions");

    auto unknownParameterEnum = *parsed.manifest;
    unknownParameterEnum.nodes.front().parameters.front().kind =
        static_cast<talas::core::ParameterKind>(255);
    const auto unknown =
        bundle::writeCanonicalComponentBundleManifest(unknownParameterEnum);
    require(
        !unknown.ok()
            && std::ranges::any_of(
                unknown.errors,
                [](const auto& error) {
                    return error.code
                        == bundle::ComponentBundleManifestErrorCode::InvalidEnum;
                }),
        "writer validation should reject caller-constructed unknown enums");

    auto unknownPortEnum = *parsed.manifest;
    unknownPortEnum.nodes.front().inputs.front().signal.audioLayout =
        static_cast<talas::core::AudioBusLayoutKind>(255);
    require(
        !bundle::writeCanonicalComponentBundleManifest(unknownPortEnum).ok(),
        "writer validation should reject caller-constructed port enums");

    auto invalidUtf8 = *parsed.manifest;
    invalidUtf8.provenance.publisher = std::string{
        static_cast<char>(0xc0),
        static_cast<char>(0xaf),
    };
    require(
        !bundle::writeCanonicalComponentBundleManifest(invalidUtf8).ok(),
        "writer validation should reject malformed UTF-8 from typed callers");

    auto oversizedCanonical = *parsed.manifest;
    oversizedCanonical.provenance.notices.assign(
        bundle::maximumComponentBundleNotices,
        std::string(
            bundle::maximumComponentBundleProvenanceTextBytes,
            static_cast<char>(0x01)));
    const auto oversized =
        bundle::writeCanonicalComponentBundleManifest(oversizedCanonical);
    require(
        !oversized.ok() && oversized.canonicalJson.empty()
            && std::ranges::any_of(
                oversized.errors,
                [](const auto& error) {
                    return error.code
                        == bundle::ComponentBundleManifestErrorCode::BudgetExceeded;
                }),
        "writer should reject an oversized escaped encoding before constructing it");
}

void testManifestInputBudgetRejectsBeforeJsonGrowth()
{
    std::string oversized(
        bundle::maximumComponentBundleManifestBytes + 1U,
        ' ');
    const auto parsed = bundle::parseComponentBundleManifest(oversized);
    require(
        !parsed.ok()
            && hasCode(parsed, bundle::ComponentBundleManifestErrorCode::BudgetExceeded),
        "manifest input should reject at its explicit byte budget");
}

void testBoundedAudioStreamRoleAndPermissionRoundTripExactly()
{
    auto json = validManifestJson();
    const auto roles = json.find(
        R"json(["realtime-processor", "declarative-ui-provider"])json");
    const auto permissions = json.find(
        R"json(["realtime-audio", "declarative-ui"])json");
    require(roles != std::string::npos && permissions != std::string::npos,
        "bounded Audio manifest fixture should find closed arrays");
    json.replace(
        roles,
        std::string_view{
            R"json(["realtime-processor", "declarative-ui-provider"])json"}
            .size(),
        R"json(["realtime-processor", "declarative-ui-provider", "bounded-audio-stream-consumer"])json");
    const auto adjustedPermissions = json.find(
        R"json(["realtime-audio", "declarative-ui"])json");
    json.replace(
        adjustedPermissions,
        std::string_view{R"json(["realtime-audio", "declarative-ui"])json"}
            .size(),
        R"json(["realtime-audio", "declarative-ui", "bounded-audio-stream-read"])json");

    const auto parsed = bundle::parseComponentBundleManifest(json);
    require(
        parsed.ok()
            && (parsed.manifest->declaredRoles
                & talas::runtime::componentRole(
                    talas::runtime::ComponentExecutionRole::
                        BoundedAudioStreamConsumer))
                != 0
            && (parsed.manifest->requestedPermissions
                & talas::runtime::componentPermission(
                    talas::runtime::ComponentExecutionPermission::
                        BoundedAudioStreamRead))
                != 0,
        "bounded Audio role and permission tokens should parse exactly");
    const auto canonical = bundle::writeCanonicalComponentBundleManifest(
        *parsed.manifest);
    require(
        canonical.ok()
            && canonical.canonicalJson.find(
                R"json("bounded-audio-stream-consumer")json")
                != std::string::npos
            && canonical.canonicalJson.find(
                R"json("bounded-audio-stream-read")json")
                != std::string::npos,
        "bounded Audio role and permission tokens should canonicalize exactly");
}
}

int main()
{
    testCompleteManifestParsesAndCanonicalizes();
    testUnknownFieldsAndNoncanonicalIntegersReject();
    testIdentityVersionRoleAndPermissionRulesRejectAmbiguity();
    testPayloadPathsDigestsAndUiReferenceAreClosed();
    testNodeSchemaDependenciesAndMigrationsRejectDuplicates();
    testWriterRejectsCallerConstructedInvalidModels();
    testManifestInputBudgetRejectsBeforeJsonGrowth();
    testBoundedAudioStreamRoleAndPermissionRoundTripExactly();
    return 0;
}
