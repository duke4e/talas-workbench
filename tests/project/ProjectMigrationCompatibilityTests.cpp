#include "JsonValue.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"

#include <charconv>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef TALAS_PROJECT_MIGRATION_POLICY_PATH
#error "TALAS_PROJECT_MIGRATION_POLICY_PATH must name the checked-in migration policy"
#endif

namespace
{
using talas::project::json::Value;

struct MigrationPolicy
{
    int currentVersion = 0;
    std::vector<int> acceptedSourceVersions;
    std::vector<int> rejectedBoundaryVersions;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::string readTextFile(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "migration policy should be readable");
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

[[nodiscard]] int exactInteger(const Value& value, std::string_view field)
{
    require(value.isNumber(), std::string(field) + " should be numeric");
    int parsed = 0;
    const auto& token = value.numberToken();
    const auto [end, error] = std::from_chars(
        token.data(),
        token.data() + token.size(),
        parsed);
    require(
        error == std::errc{} && end == token.data() + token.size(),
        std::string(field) + " should be an exact decimal integer");
    return parsed;
}

[[nodiscard]] const Value& requiredMember(
    const Value& object,
    std::string_view key)
{
    const auto* member = talas::project::json::findObjectMember(object, key);
    require(member != nullptr, "migration policy is missing " + std::string(key));
    return *member;
}

[[nodiscard]] std::vector<int> exactIntegerArray(
    const Value& root,
    std::string_view key)
{
    const auto& source = requiredMember(root, key);
    require(source.isArray(), std::string(key) + " should be an array");
    std::vector<int> values;
    values.reserve(source.array().size());
    for (const auto& value : source.array())
        values.push_back(exactInteger(value, key));
    return values;
}

[[nodiscard]] MigrationPolicy parsePolicy(std::string_view text)
{
    const auto parsed = talas::project::json::parse(text);
    require(parsed.ok(), "migration policy should be strict valid JSON");
    require(parsed.value.isObject(), "migration policy should be an object");
    require(parsed.value.object().size() == 5, "migration policy should have five exact keys");

    require(
        exactInteger(requiredMember(parsed.value, "schemaVersion"), "schemaVersion") == 1,
        "migration policy schema should be version one");
    const auto& format = requiredMember(parsed.value, "format");
    require(
        format.isString() && format.string() == "talas.project",
        "migration policy should govern talas.project");

    MigrationPolicy policy;
    policy.currentVersion = exactInteger(
        requiredMember(parsed.value, "currentVersion"),
        "currentVersion");
    policy.acceptedSourceVersions = exactIntegerArray(
        parsed.value,
        "acceptedSourceVersions");
    policy.rejectedBoundaryVersions = exactIntegerArray(
        parsed.value,
        "rejectedBoundaryVersions");

    require(policy.currentVersion > 0, "current version should be positive");
    require(
        policy.acceptedSourceVersions.size()
            == static_cast<std::size_t>(policy.currentVersion),
        "accepted source versions should cover every version through current");
    for (int index = 0; index < policy.currentVersion; ++index)
    {
        require(
            policy.acceptedSourceVersions[static_cast<std::size_t>(index)]
                == index + 1,
            "accepted source versions should be unique, ordered, and contiguous");
    }
    require(
        policy.rejectedBoundaryVersions
            == std::vector<int>{0, policy.currentVersion + 1},
        "rejected boundaries should be exactly zero and current plus one");
    return policy;
}

template <typename Callback>
void requireThrows(Callback&& callback, const std::string& message)
{
    try
    {
        callback();
    }
    catch (const std::runtime_error&)
    {
        return;
    }
    throw std::runtime_error(message);
}

[[nodiscard]] std::string minimalProjectJson(
    int version,
    bool injectFuturePresentation = false)
{
    std::string output = "{\n"
        "  \"format\": \"talas.project\",\n"
        "  \"version\": " + std::to_string(version);
    if (version == 23)
        output += ",\n  \"expressionLanguage\": \"pureV2\"";
    else if (version == 24)
        output += ",\n  \"expressionLanguage\": \"pureV3\"";
    else if (version == 25)
    {
        output += ",\n  \"expressionLanguage\": \"pureV4\"";
        output += ",\n  \"expressionDefinitions\": []";
    }
    else if (version == 26)
    {
        output += ",\n  \"expressionLanguage\": \"sessionV1\"";
        output += ",\n  \"expressionDefinitions\": []";
    }
    else if (version >= 27)
    {
        output += version >= 35
            ? ",\n  \"expressionLanguage\": \"variablesV2\""
            : ",\n  \"expressionLanguage\": \"variablesV1\"";
        output += ",\n  \"expressionDefinitions\": []";
        output += ",\n  \"variableLibrary\": {"
            " \"version\": " + std::to_string(version == 27 ? 1 : 2)
            + ", \"variables\": [] }";
    }
    if (version >= 29)
    {
        output += ",\n  \"soundVariableLibrary\": {"
            " \"version\": 1, \"variables\": [] }";
    }
    if (version >= 30)
    {
        output += ",\n  \"generatorLibrary\": {"
            " \"version\": " + std::to_string(version == 30 ? 1 : 2)
            + ", \"programs\": [] }";
    }
    if (version >= 34)
    {
        output += ",\n  \"liveSetLibrary\": {"
            " \"version\": 1, \"liveSets\": [], \"tools\": [] }";
    }
    output += ",\n  \"metadata\": { \"title\": \"Migration v"
        + std::to_string(version) + "\" }";

    if (version >= 4)
        output += ",\n  \"assets\": []";
    if (version >= 22)
        output += ",\n  \"partialTrackEdits\": []";
    if (version >= 3)
    {
        output += ",\n  \"timeline\": {";
        if (version >= 6)
        {
            output += "\n    \"loop\": { \"enabled\": false, "
                "\"startSeconds\": 0, \"endSeconds\": 4 },";
        }
        if (version >= 16)
        {
            output += "\n    \"musicalTime\": {"
                " \"ticksPerQuarter\": 960,"
                " \"tempo\": [{ \"tick\": 0, \"microsecondsPerQuarter\": 500000 }],"
                " \"meter\": [{ \"tick\": 0, \"numerator\": 4, \"denominator\": 4 }] },";
        }
        if (version >= 21)
            output += "\n    \"score\": { \"tracks\": [], \"clips\": [], \"automation\": [], \"markers\": [] },";
        output += "\n    \"eventValues\": []\n  }";
    }
    if (version >= 5)
    {
        output += R"json(,
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
  "outputChannels": [],
  "midiInputDevices": []
  })json";
        if (version >= 15)
        {
            const auto deviceEnd = output.rfind("\n  }");
            output.insert(
                deviceEnd,
                version >= 17
                    ? ",\n    \"transportSync\": { \"sourceMode\": \"internal\", \"sourceIdentifier\": \"\" }"
                    : ",\n    \"midiClock\": { \"sourceMode\": \"internal\", \"sourceIdentifier\": \"\" }");
            if (version >= 18)
            {
                const auto currentDeviceEnd = output.rfind("\n  }");
                output.insert(
                    currentDeviceEnd,
                    ",\n    \"oscIngress\": { \"enabled\": false, \"port\": 9000 }");
            }
            if (version >= 19)
            {
                const auto currentDeviceEnd = output.rfind("\n  }");
                output.insert(
                    currentDeviceEnd,
                    ",\n    \"oscEgress\": { \"enabled\": false, \"port\": 9001 }");
            }
        }
    }
    if (version >= 8)
        output += ",\n  \"soundDefinitions\": []";
    if (version >= 11 || injectFuturePresentation)
    {
        output += version >= 20 ? R"json(,
  "presentation": {
    "version": 2,
    "structureLayouts": [
      { "scope": { "kind": "project" }, "nodes": [] }
    ],
    "performanceControls": [],
    "performanceMeters": []
  },
  "snapshotLibrary": { "version": 1, "banks": [] })json"
        : R"json(,
  "presentation": {
    "version": 1,
    "structureLayouts": [
      { "scope": { "kind": "project" }, "nodes": [] }
    ],
    "performanceControls": []
  },
  "snapshotLibrary": { "version": 1, "banks": [] })json";
    }
    if (version >= 12)
        output += ",\n  \"sampleMapLibrary\": { \"version\": 1, \"maps\": [] }";

    output += ",\n  \"graph\": { \"nodes\": [], \"connections\": [] }\n}";
    return output;
}

void requireErrorAt(
    std::string_view source,
    std::string_view expectedPath,
    const std::string& message)
{
    const auto result = talas::project::parseProjectDocument(source);
    require(!result.ok(), message);
    for (const auto& error : result.errors)
    {
        if (error.path == expectedPath)
            return;
    }
    throw std::runtime_error(message + " should report " + std::string(expectedPath));
}

void requireNoInventedState(
    const talas::project::ProjectDocument& document,
    int sourceVersion)
{
    require(
        document.metadata.title == "Migration v" + std::to_string(sourceVersion),
        "migration should preserve project metadata");
    require(document.graph.nodes.empty(), "migration should not invent nodes");
    require(document.graph.connections.empty(), "migration should not invent connections");
    require(document.virtualControls.empty(), "migration should not invent controls");
    require(document.assets.empty(), "migration should not invent assets");
    require(document.partialTrackEdits.empty(), "migration should not invent partial-track edits");
    require(document.timeline.eventValues.empty(), "migration should not invent events");
    require(document.timeline.score == talas::project::ProjectScore{},
            "migration should not invent score entities");
    require(document.timeline.musicalTime
                == talas::project::ProjectMusicalTime{},
            "migration should establish only the canonical 120 BPM and 4/4 anchors");
    require(!document.deviceSettings.configured, "migration should not configure a device");
    require(document.deviceSettings.audioInputDeviceName.empty(), "migration should not invent an input device");
    require(document.deviceSettings.audioOutputDeviceName.empty(), "migration should not invent an output device");
    require(document.deviceSettings.sampleRate == 0.0, "migration should not invent a sample rate");
    require(document.deviceSettings.bufferSize == 0, "migration should not invent a buffer size");
    require(document.deviceSettings.inputChannels.empty(), "migration should not invent input channels");
    require(document.deviceSettings.outputChannels.empty(), "migration should not invent output channels");
    require(document.deviceSettings.midiInputDeviceIdentifiers.empty(), "migration should not invent MIDI devices");
    require(document.deviceSettings.transportSync.sourceMode
                == talas::project::TransportSyncSourceMode::Internal
            && document.deviceSettings.transportSync.sourceIdentifier.empty(),
            "migration should not invent MIDI Clock authority");
    require(!document.deviceSettings.oscIngress.enabled
                && document.deviceSettings.oscIngress.port == 9000,
            "migration should not enable OSC or invent a port");
    require(!document.deviceSettings.oscEgress.enabled
                && document.deviceSettings.oscEgress.port == 9001,
            "migration should not enable OSC feedback or invent a destination");
    require(document.soundDefinitions.empty(), "migration should not invent Sounds");
    require(document.snapshotLibrary.banks.empty(), "migration should not invent snapshots");
    require(document.sampleMapLibrary.maps.empty(), "migration should not invent sample maps");
    require(
        document.expressionDefinitions.definitions.empty(),
        "migration should not invent expression definitions");
    require(
        document.variableLibrary.variables.empty(),
        "migration should not invent variable declarations");
    require(
        document.soundVariableLibrary.variables.empty(),
        "migration should not invent Sound-variable declarations");
    require(
        document.generatorLibrary.programs.empty(),
        "migration should not invent generator programs");
    require(
        document.liveSetLibrary.liveSets.empty()
            && document.liveSetLibrary.tools.empty(),
        "migration should not invent live sets or tools");
    require(document.presentation.performanceControls.empty(), "migration should not invent presented controls");
    for (const auto& layout : document.presentation.structureLayouts)
        require(layout.nodes.empty(), "migration should not invent node positions");
}

[[nodiscard]] talas::expr::ExpressionLanguageVersion
expectedExpressionLanguage(int sourceVersion)
{
    if (sourceVersion <= 22)
        return talas::expr::ExpressionLanguageVersion::LegacyV1;
    if (sourceVersion == 23)
        return talas::expr::ExpressionLanguageVersion::PureV2;
    if (sourceVersion == 24)
        return talas::expr::ExpressionLanguageVersion::PureV3;
    if (sourceVersion == 25)
        return talas::expr::ExpressionLanguageVersion::PureV4;
    if (sourceVersion == 26)
        return talas::expr::ExpressionLanguageVersion::SessionV1;
    if (sourceVersion <= 34)
        return talas::expr::ExpressionLanguageVersion::VariablesV1;
    return talas::expr::ExpressionLanguageVersion::VariablesV2;
}

[[nodiscard]] std::string_view expectedExpressionLanguageName(
    int sourceVersion)
{
    if (sourceVersion <= 22)
        return "legacyV1";
    if (sourceVersion == 23)
        return "pureV2";
    if (sourceVersion == 24)
        return "pureV3";
    if (sourceVersion == 25)
        return "pureV4";
    if (sourceVersion == 26)
        return "sessionV1";
    return sourceVersion <= 34 ? "variablesV1" : "variablesV2";
}

void testPolicyValidationIsClosed()
{
    const auto policy = parsePolicy(readTextFile(TALAS_PROJECT_MIGRATION_POLICY_PATH));
    require(policy.currentVersion == 156, "checked-in current project version should be 156");
    require(policy.acceptedSourceVersions.front() == 1, "accepted boundary should start at one");
    require(policy.acceptedSourceVersions.back() == 156, "accepted boundary should end at current");

    const std::vector<std::string> invalidPolicies{
        R"json({"schemaVersion":1,"format":"talas.project","acceptedSourceVersions":[1],"rejectedBoundaryVersions":[0,2]})json",
        R"json({"schemaVersion":1,"format":"talas.project","currentVersion":1,"acceptedSourceVersions":[1],"rejectedBoundaryVersions":[0,2],"extra":true})json",
        R"json({"schemaVersion":1,"schemaVersion":1,"format":"talas.project","currentVersion":1,"acceptedSourceVersions":[1],"rejectedBoundaryVersions":[0,2]})json",
        R"json({"schemaVersion":1,"format":"talas.project","currentVersion":1,"acceptedSourceVersions":[1.0],"rejectedBoundaryVersions":[0,2]})json",
        R"json({"schemaVersion":1,"format":"talas.project","currentVersion":2,"acceptedSourceVersions":[1,1],"rejectedBoundaryVersions":[0,3]})json",
        R"json({"schemaVersion":1,"format":"talas.project","currentVersion":3,"acceptedSourceVersions":[1,3,2],"rejectedBoundaryVersions":[0,4]})json",
        R"json({"schemaVersion":1,"format":"talas.project","currentVersion":1,"acceptedSourceVersions":[0],"rejectedBoundaryVersions":[0,2]})json",
    };
    for (const auto& invalid : invalidPolicies)
    {
        requireThrows(
            [&] { static_cast<void>(parsePolicy(invalid)); },
            "invalid migration policy should fail closed");
    }
}

void testEveryAcceptedVersionMigratesCanonically()
{
    const auto policy = parsePolicy(readTextFile(TALAS_PROJECT_MIGRATION_POLICY_PATH));
    require(
        talas::project::ProjectDocument{}.formatVersion == policy.currentVersion,
        "in-memory default should agree with migration policy");

    for (const auto sourceVersion : policy.acceptedSourceVersions)
    {
        const auto loaded = talas::project::parseProjectDocument(
            minimalProjectJson(sourceVersion));
        if (!loaded.ok())
        {
            std::string detail;
            for (const auto& error : loaded.errors)
                detail += " [" + error.path + ": " + error.message + "]";
            throw std::runtime_error(
                "accepted migration source should parse: v"
                + std::to_string(sourceVersion) + detail);
        }
        require(
            loaded.document.formatVersion == policy.currentVersion,
            "accepted source should migrate to current in memory");
        require(
            loaded.document.expressionLanguage
                == expectedExpressionLanguage(sourceVersion),
            "accepted source should retain exact migration dialect authority");
        requireNoInventedState(loaded.document, sourceVersion);

        const auto canonical = talas::project::serializeProjectDocument(loaded.document);
        const auto generic = talas::project::json::parse(canonical);
        require(generic.ok(), "canonical project should remain strict JSON");
        require(
            exactInteger(requiredMember(generic.value, "version"), "version")
                == policy.currentVersion,
            "canonical writer should emit the policy current version");
        const auto& language = requiredMember(
            generic.value,
            "expressionLanguage");
        require(
            language.isString()
                && language.string()
                    == expectedExpressionLanguageName(sourceVersion),
            "canonical writer should retain exact expression dialect");

        const auto reopened = talas::project::parseProjectDocument(canonical);
        require(reopened.ok(), "canonical migrated project should reopen");
        require(
            reopened.document.expressionLanguage
                == loaded.document.expressionLanguage,
            "reopen should preserve the migrated expression dialect");
        requireNoInventedState(reopened.document, sourceVersion);
        require(
            talas::project::serializeProjectDocument(reopened.document) == canonical,
            "second canonical serialization should be byte-stable");
    }
}

// Every format from 42 on keeps a Val project's expression language;
// those of formats 96 to 98 and 136 to 152 once fell back to the format's
// default.
void testEveryValVersionKeepsItsLanguage()
{
    const auto policy = parsePolicy(readTextFile(TALAS_PROJECT_MIGRATION_POLICY_PATH));
    for (const auto sourceVersion : policy.acceptedSourceVersions)
    {
        if (sourceVersion < 42)
            continue;
        auto source = minimalProjectJson(sourceVersion);
        const std::string variables = "\"expressionLanguage\": \"variablesV2\"";
        const auto position = source.find(variables);
        require(position != std::string::npos, "the fixture should name its expression language");
        source.replace(position, variables.size(), "\"expressionLanguage\": \"valV1\"");
        const auto loaded = talas::project::parseProjectDocument(source);
        require(loaded.ok() && loaded.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1,
            "a Val project of format " + std::to_string(sourceVersion) + " should keep its expression language");
    }
}

void testUnsupportedAndMalformedVersionsFailClosed()
{
    const auto policy = parsePolicy(readTextFile(TALAS_PROJECT_MIGRATION_POLICY_PATH));
    auto mathematicallyIntegral = minimalProjectJson(1);
    const auto integralVersion = mathematicallyIntegral.find("\"version\": 1");
    require(integralVersion != std::string::npos, "version-one row should expose its token");
    mathematicallyIntegral.replace(
        integralVersion,
        std::string_view{"\"version\": 1"}.size(),
        "\"version\": 1.0");
    require(
        talas::project::parseProjectDocument(mathematicallyIntegral).ok(),
        "mathematically integral decimal versions should remain compatible");

    for (const auto version : policy.rejectedBoundaryVersions)
    {
        requireErrorAt(
            minimalProjectJson(version),
            "$.version",
            "unsupported boundary version should fail");
    }

    requireErrorAt(
        R"json({"format":"talas.project","metadata":{"title":"Missing"},"graph":{"nodes":[],"connections":[]}})json",
        "$.version",
        "missing version should fail");
    requireErrorAt(
        R"json({"format":"talas.project","version":1.5,"metadata":{"title":"Fractional"},"graph":{"nodes":[],"connections":[]}})json",
        "$.version",
        "fractional version should fail");
    requireErrorAt(
        R"json({"format":"talas.project","version":1.00000000000000001,"metadata":{"title":"Rounded fractional"},"graph":{"nodes":[],"connections":[]}})json",
        "$.version",
        "fractional token that rounds to an accepted double should fail");
    requireErrorAt(
        R"json({"format":"talas.project","version":1,"version":1,"metadata":{"title":"Duplicate"},"graph":{"nodes":[],"connections":[]}})json",
        "$",
        "duplicate version should fail");
    requireErrorAt(
        minimalProjectJson(10, true),
        "$.presentation",
        "pre-v11 source should reject future presentation state");
    auto prematurePartialEdits = minimalProjectJson(21);
    const auto graph = prematurePartialEdits.find("  \"graph\":");
    require(graph != std::string::npos,
            "format-21 row should expose its graph boundary");
    prematurePartialEdits.insert(
        graph,
        "  \"partialTrackEdits\": [],\n");
    requireErrorAt(
        prematurePartialEdits,
        "$.partialTrackEdits",
        "pre-v22 source should reject future partial-track edit authority");

    auto prematureExpressionLanguage = minimalProjectJson(22);
    const auto metadata = prematureExpressionLanguage.find("  \"metadata\":");
    require(metadata != std::string::npos,
            "format-22 row should expose its metadata boundary");
    prematureExpressionLanguage.insert(
        metadata,
        "  \"expressionLanguage\": \"pureV2\",\n");
    requireErrorAt(
        prematureExpressionLanguage,
        "$.expressionLanguage",
        "pre-v23 source should reject future expression authority");

    auto prematureVariablesV2 = minimalProjectJson(34);
    const auto variablesV1 = prematureVariablesV2.find("variablesV1");
    require(
        variablesV1 != std::string::npos,
        "format-34 row should expose frozen VariablesV1 authority");
    prematureVariablesV2.replace(
        variablesV1,
        std::string_view{"variablesV1"}.size(),
        "variablesV2");
    requireErrorAt(
        prematureVariablesV2,
        "$.expressionLanguage",
        "format 34 should reject premature VariablesV2 authority");
}
}

int main()
{
    testPolicyValidationIsClosed();
    testEveryAcceptedVersionMigratesCanonically();
    testEveryValVersionKeepsItsLanguage();
    testUnsupportedAndMalformedVersionsFailClosed();
    return 0;
}
