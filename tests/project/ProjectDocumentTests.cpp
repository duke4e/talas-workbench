#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/VirtualControlMidi.h"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

template <typename Value>
concept HasInlineCanvasPosition = requires(Value value) {
    value.position;
};

static_assert(!HasInlineCanvasPosition<talas::project::ProjectNode>);
static_assert(!HasInlineCanvasPosition<talas::project::VirtualControl>);

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::CanvasPosition nodePosition(
    const talas::project::ProjectDocument& document,
    std::string_view nodeId,
    std::optional<std::string_view> soundDefinitionId = std::nullopt)
{
    const auto* presented = talas::project::findProjectNodePresentation(
        document,
        soundDefinitionId,
        nodeId);
    require(presented != nullptr, "expected node presentation entry");
    return presented->position;
}

talas::project::CanvasPosition controlPosition(
    const talas::project::ProjectDocument& document,
    std::string_view eventName,
    talas::expr::ValueDomain domain)
{
    const auto* presented =
        talas::project::findProjectPerformanceControlPresentation(
            document,
            eventName,
            domain);
    require(presented != nullptr, "expected Performance presentation entry");
    return presented->position;
}

void requireContains(std::string_view haystack, std::string_view needle, const std::string& message)
{
    require(haystack.find(needle) != std::string_view::npos, message);
}

void requireNotContains(std::string_view haystack, std::string_view needle, const std::string& message)
{
    require(haystack.find(needle) == std::string_view::npos, message);
}

void requireLoadError(std::string_view json, const std::string& message)
{
    const auto result = talas::project::parseProjectDocument(json);
    require(!result.ok(), message);
    require(!result.errors.front().message.empty(), "load errors should include a message");
    require(!result.errors.front().path.empty(), "load errors should include a path");
}

void requireLoadErrorAt(
    std::string_view json,
    std::string_view expectedPath,
    const std::string& message)
{
    const auto result = talas::project::parseProjectDocument(json);
    require(!result.ok(), message);
    require(
        std::ranges::any_of(result.errors, [&](const auto& error) {
            return error.path == expectedPath;
        }),
        message + " should report " + std::string(expectedPath));
}

std::string_view domainName(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
        return "scalar";
    case talas::expr::ValueDomain::Hertz:
        return "hertz";
    case talas::expr::ValueDomain::Seconds:
        return "seconds";
    case talas::expr::ValueDomain::NoteNumber:
        return "noteNumber";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    }
    return {};
}

void downgradePresentationToV1(
    std::string& source,
    const talas::project::ProjectDocument& document)
{
    const auto presentation = source.find("  \"presentation\": {\n");
    const auto version = source.find("    \"version\": 2,\n", presentation);
    const auto controls = source.find(
        "    \"performanceControls\": ", presentation);
    const auto meters = source.find(
        "    \"performanceMeters\": []\n", controls);
    require(presentation != std::string::npos
            && version != std::string::npos
            && controls != std::string::npos
            && meters != std::string::npos,
        "current fixture should contain canonical presentation v2 without meters");

    std::string legacy = "    \"performanceControls\": [";
    bool first = true;
    for (const auto& control : document.presentation.performanceControls)
    {
        require(control.target.kind
                    == talas::project::SnapshotTargetKind::EventValue,
            "legacy presentation fixture can contain Event Value controls only");
        legacy += first ? "\n" : ",\n";
        first = false;
        legacy += "      {\n";
        legacy += "        \"eventName\": \"" + control.target.eventName
            + "\",\n";
        legacy += "        \"domain\": \""
            + std::string{domainName(control.target.domain)} + "\",\n";
        legacy += "        \"x\": " + std::to_string(control.position.x)
            + ",\n";
        legacy += "        \"y\": " + std::to_string(control.position.y)
            + ",\n";
        legacy += std::string{"        \"visible\": "}
            + (control.visible ? "true\n" : "false\n");
        legacy += "      }";
    }
    if (!first)
        legacy += "\n";
    legacy += "    ]\n";

    source.erase(meters,
        std::string{"    \"performanceMeters\": []\n"}.size());
    source.replace(controls, meters - controls, legacy);
    source.replace(version,
        std::string{"    \"version\": 2,\n"}.size(),
        "    \"version\": 1,\n");
}

void eraseFormatSeventeenTransportSync(std::string& source)
{
    const std::string field = R"json(,
    "transportSync": {
      "sourceMode": "internal",
      "sourceIdentifier": ""
    })json";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should contain format-seventeen transport sync authority");
    source.erase(position, field.size());
}

void eraseFormatEighteenOscIngress(std::string& source)
{
    const std::string egressField = R"json(,
    "oscEgress": {
      "enabled": false,
      "port": 9001
    })json";
    const auto egressPosition = source.find(egressField);
    require(egressPosition != std::string::npos,
            "fixture should contain format-nineteen OSC feedback settings");
    source.erase(egressPosition, egressField.size());
    const std::string field = R"json(,
    "oscIngress": {
      "enabled": false,
      "port": 9000
    })json";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should contain format-eighteen OSC ingress settings");
    source.erase(position, field.size());
}

void eraseFormatSixteenMusicalTime(std::string& source)
{
    const auto start = source.find("    \"musicalTime\": {\n");
    const auto end = source.find("    \"eventValues\": ", start);
    require(start != std::string::npos && end != std::string::npos,
            "fixture should contain format-sixteen musical time");
    source.erase(start, end - start);
    const auto edits = source.find("  \"partialTrackEdits\": [],\n");
    if (edits != std::string::npos)
    {
        source.erase(
            edits,
            std::string_view{"  \"partialTrackEdits\": [],\n"}.size());
    }
}

void eraseVariableLibrary(std::string& source);

void eraseLiveSetLibrary(std::string& source)
{
    constexpr std::string_view field =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto position = source.find(field);
    require(
        position != std::string::npos,
        "current fixture should contain an empty live-set library");
    source.erase(position, field.size());
}

void eraseExpressionLanguage(std::string& source)
{
    constexpr std::array<std::string_view, 7> fields{
        "  \"expressionLanguage\": \"legacyV1\",\n",
        "  \"expressionLanguage\": \"pureV2\",\n",
        "  \"expressionLanguage\": \"pureV3\",\n",
        "  \"expressionLanguage\": \"pureV4\",\n",
        "  \"expressionLanguage\": \"sessionV1\",\n",
        "  \"expressionLanguage\": \"variablesV1\",\n",
        "  \"expressionLanguage\": \"variablesV2\",\n",
    };
    auto position = std::string::npos;
    std::size_t fieldSize = 0;
    for (const auto field : fields)
    {
        position = source.find(field);
        if (position != std::string::npos)
        {
            fieldSize = field.size();
            break;
        }
    }
    require(
        position != std::string::npos,
        "current fixture should contain expression-language authority");
    source.erase(position, fieldSize);
    constexpr std::string_view definitions =
        "  \"expressionDefinitions\": [],\n";
    if (const auto definitionsPosition = source.find(definitions);
        definitionsPosition != std::string::npos)
    {
        source.erase(definitionsPosition, definitions.size());
    }
    eraseVariableLibrary(source);
}

void eraseVariableLibrary(std::string& source)
{
    constexpr std::string_view field =
        "  \"variableLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto position = source.find(field);
    require(
        position != std::string::npos,
        "current fixture should contain an empty variable library");
    source.erase(position, field.size());
    constexpr std::string_view soundField =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto soundPosition = source.find(soundField);
    require(
        soundPosition != std::string::npos,
        "current fixture should contain an empty Sound-variable library");
    source.erase(soundPosition, soundField.size());
    constexpr std::string_view generatorField =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    const auto generatorPosition = source.find(generatorField);
    require(
        generatorPosition != std::string::npos,
        "current fixture should contain an empty generator library");
    source.erase(generatorPosition, generatorField.size());
    eraseLiveSetLibrary(source);
}

void eraseEmptyExpressionDefinitions(std::string& source)
{
    constexpr std::string_view field =
        "  \"expressionDefinitions\": [],\n";
    const auto position = source.find(field);
    require(
        position != std::string::npos,
        "current fixture should contain empty expression definitions");
    source.erase(position, field.size());
    eraseVariableLibrary(source);
}

std::string projectWithSoundDefinitions(std::string_view definitions)
{
    return std::string{R"json({
  "format": "talas.project",
  "version": 8,
  "metadata": { "title": "Nested" },
  "virtualControls": [],
  "assets": [],
  "timeline": {
    "loop": { "enabled": false, "startSeconds": 0, "endSeconds": 4 },
    "eventValues": []
  },
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
    "outputChannels": [],
    "midiInputDevices": []
  },
  "soundDefinitions": )json"}
        + std::string{definitions}
        + R"json(,
  "graph": { "nodes": [], "connections": [] }
})json";
}

std::string versionNineProjectWithSoundDefinitions(
    std::string_view definitions)
{
    auto project = projectWithSoundDefinitions(definitions);
    const auto version = project.find(R"json("version": 8)json");
    require(version != std::string::npos,
            "Sound fixture should contain its format version");
    project.replace(
        version,
        std::string{R"json("version": 8)json"}.size(),
        R"json("version": 9)json");
    return project;
}

talas::project::ProjectParameterExpression choiceParameter(std::string id, std::string choice)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    return parameter;
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(std::string id, bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndChoiceParameter(std::string id,
                                                                     std::string expression,
                                                                     std::string choice)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.choice = std::move(choice);
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndBooleanParameter(std::string id,
                                                                      std::string expression,
                                                                      bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression expressionAndResourceParameter(std::string id,
                                                                       std::string expression,
                                                                       std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectParameterExpression booleanAndTextParameter(std::string id,
                                                                bool value,
                                                                std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    parameter.text = std::move(text);
    return parameter;
}

talas::project::ProjectParameterExpression choiceAndBooleanParameter(std::string id,
                                                                  std::string choice,
                                                                  bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(choice);
    parameter.boolean = value;
    return parameter;
}

void testEmptyProjectSerializesToCanonicalJson()
{
    const talas::project::ProjectDocument document;

    const std::string expected = R"json({
  "format": "talas.project",
  "version": 156,
  "expressionLanguage": "variablesV2",
  "expressionDefinitions": [],
  "variableLibrary": {
    "version": 2,
    "variables": []
  },
  "soundVariableLibrary": {
    "version": 1,
    "variables": []
  },
  "generatorLibrary": {
    "version": 2,
    "programs": []
  },
  "liveSetLibrary": {
    "version": 1,
    "liveSets": [],
    "tools": []
  },
  "metadata": {
    "title": ""
  },
  "presentation": {
    "version": 2,
    "structureLayouts": [
      {
        "scope": {
          "kind": "project"
        },
        "nodes": []
      }
    ],
    "performanceControls": [],
    "performanceMeters": []
  },
  "snapshotLibrary": {
    "version": 1,
    "banks": []
  },
  "sampleMapLibrary": {
    "version": 1,
    "maps": []
  },
  "virtualControls": [],
  "assets": [],
  "partialTrackEdits": [],
  "timeline": {
    "loop": {
      "enabled": false,
      "startSeconds": 0,
      "endSeconds": 4
    },
    "musicalTime": {
      "ticksPerQuarter": 960,
      "tempo": [
        {
          "tick": 0,
          "microsecondsPerQuarter": 500000
        }
      ],
      "meter": [
        {
          "tick": 0,
          "numerator": 4,
          "denominator": 4
        }
      ]
    },
    "score": {
      "tracks": [
      ],
      "clips": [
      ],
      "automation": [
      ],
      "markers": [
      ]
    },
    "eventValues": []
  },
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
    "outputChannels": [],
    "midiInputDevices": [],
    "transportSync": {
      "sourceMode": "internal",
      "sourceIdentifier": ""
    },
    "oscIngress": {
      "enabled": false,
      "port": 9000
    },
    "oscEgress": {
      "enabled": false,
      "port": 9001
    }
  },
  "soundDefinitions": [],
  "graph": {
    "nodes": [],
    "connections": []
  }
})json";

    require(talas::project::serializeProjectDocument(document) == expected, "empty project should serialize to canonical JSON");
}

void testFormatTwentySevenOwnsVariableExpressions()
{
    talas::project::ProjectDocument pure;
    require(
        pure.formatVersion == talas::project::currentProjectFormatVersion
            && pure.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2,
        "new documents should default to format-35 VariablesV2 authority");
    pure.expressionDefinitions.definitions.push_back({
        "scale",
        {{"value", talas::expr::ValueDomain::Scalar},
         {"amount", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value * amount",
    });
    pure.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expressionParameter("value", "scale(series(1, 2, 3)[1], 0.5)")},
    });
    talas::project::reconcileProjectPresentation(pure);
    const auto canonical =
        talas::project::serializeProjectDocument(pure);
    requireContains(
        canonical,
        R"json("expressionLanguage": "variablesV2")json",
        "format 35 should persist exact VariablesV2 authority");
    requireContains(
        canonical,
        R"json("name": "scale")json",
        "format 26 should persist the expression definition library");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::VariablesV2
            && reopened.document.expressionDefinitions.definitions
                == pure.expressionDefinitions.definitions,
        "format-35 VariablesV2 calls should reopen with exact definitions");

    auto missing = canonical;
    eraseExpressionLanguage(missing);
    requireLoadErrorAt(
        missing,
        "$.expressionLanguage",
        "format 26 should require expression-language authority");

    auto missingDefinitions = canonical;
    const auto definitionsBegin = missingDefinitions.find(
        "  \"expressionDefinitions\": [\n");
    const auto metadataBegin = missingDefinitions.find(
        "  \"metadata\":", definitionsBegin);
    require(
        definitionsBegin != std::string::npos
            && metadataBegin != std::string::npos,
        "format-26 fixture should expose expression definitions");
    missingDefinitions.erase(
        definitionsBegin,
        metadataBegin - definitionsBegin);
    requireLoadErrorAt(
        missingDefinitions,
        "$.expressionDefinitions",
        "format 26 should require expression definitions");

    auto unknown = canonical;
    const auto pureLanguage = unknown.find(
        R"json("expressionLanguage": "variablesV2")json");
    require(pureLanguage != std::string::npos,
        "format-26 fixture should expose expression language");
    unknown.replace(
        pureLanguage,
        std::string_view{
            R"json("expressionLanguage": "variablesV2")json"}.size(),
        R"json("expressionLanguage": "native")json");
    requireLoadErrorAt(
        unknown,
        "$.expressionLanguage",
        "unknown expression languages should fail closed");

    auto unknownDefinitionField = canonical;
    const auto definitionName = unknownDefinitionField.find(
        "      \"name\": \"scale\",\n");
    require(definitionName != std::string::npos,
        "definition fixture should expose its name");
    unknownDefinitionField.insert(
        definitionName,
        "      \"unexpected\": true,\n");
    requireLoadErrorAt(
        unknownDefinitionField,
        "$.expressionDefinitions[0]",
        "definition objects should reject unknown fields");

    auto unsupportedDomain = canonical;
    const auto scalarResult = unsupportedDomain.find(
        R"json("resultDomain": "scalar")json");
    require(scalarResult != std::string::npos,
        "definition fixture should expose its result domain");
    unsupportedDomain.replace(
        scalarResult,
        std::string_view{R"json("resultDomain": "scalar")json"}.size(),
        R"json("resultDomain": "native")json");
    requireLoadErrorAt(
        unsupportedDomain,
        "$.expressionDefinitions[0].resultDomain",
        "definition result domains should be closed");

    auto oversizedSource = canonical;
    const auto body = oversizedSource.find(
        R"json("source": "value * amount")json");
    require(body != std::string::npos,
        "definition fixture should expose its body");
    oversizedSource.replace(
        body,
        std::string_view{R"json("source": "value * amount")json"}.size(),
        "\"source\": \""
            + std::string(
                talas::expr::ExpressionLimits::maximumDefinitionSourceBytes + 1,
                'x')
            + "\"");
    requireLoadErrorAt(
        oversizedSource,
        "$",
        "definition source bounds should apply during JSON admission");

    auto prematureDefinitions = canonical;
    const auto definitionVersion = prematureDefinitions.find(
        R"json("version": 156)json");
    const auto v4Language = prematureDefinitions.find(
        R"json("expressionLanguage": "variablesV2")json");
    require(definitionVersion != std::string::npos
            && v4Language != std::string::npos,
        "definition fixture should expose format and language authority");
    prematureDefinitions.replace(
        definitionVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  24)json");
    prematureDefinitions.replace(
        v4Language,
        std::string_view{R"json("expressionLanguage": "variablesV2")json"}.size(),
        R"json("expressionLanguage": "pureV3")json");
    eraseVariableLibrary(prematureDefinitions);
    requireLoadErrorAt(
        prematureDefinitions,
        "$.expressionDefinitions",
        "non-empty definition libraries should require format-25 PureV4 authority");

    auto duplicate = canonical;
    const auto metadata = duplicate.find("  \"metadata\":");
    require(metadata != std::string::npos,
        "format-24 fixture should expose its metadata boundary");
    duplicate.insert(
        metadata,
        "  \"expressionLanguage\": \"pureV4\",\n");
    requireLoadErrorAt(
        duplicate,
        "$",
        "duplicate expression-language authority should fail closed");

    talas::project::ProjectDocument legacySource;
    legacySource.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expressionParameter("value", "1")},
    });
    talas::project::reconcileProjectPresentation(legacySource);
    auto legacyJson =
        talas::project::serializeProjectDocument(legacySource);
    const auto expression = legacyJson.find(
        R"json("expression": "1")json");
    require(expression != std::string::npos,
        "legacy fixture should expose its expression source");
    std::string longLegacy = "1";
    longLegacy.append(
        talas::expr::ExpressionLimits::maximumSourceBytes,
        ' ');
    legacyJson.replace(
        expression,
        std::string_view{R"json("expression": "1")json"}.size(),
        "\"expression\": \"" + longLegacy + "\"");
    const auto version = legacyJson.find(R"json("version": 156)json");
    require(version != std::string::npos,
        "legacy fixture should expose the current version");
    legacyJson.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  22)json");
    eraseExpressionLanguage(legacyJson);
    const auto migrated = talas::project::parseProjectDocument(legacyJson);
    require(
        migrated.ok()
            && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::LegacyV1
            && *migrated.document.graph.nodes.front()
                    .parameters.front().expression
                == longLegacy,
        "format 22 should migrate legacy expression bytes without V2 bounds");
    const auto migratedCanonical =
        talas::project::serializeProjectDocument(migrated.document);
    requireContains(
        migratedCanonical,
        R"json("expressionLanguage": "legacyV1")json",
        "migrated canonical v25 should retain explicit legacy authority");
    const auto migratedReopened =
        talas::project::parseProjectDocument(migratedCanonical);
    require(
        migratedReopened.ok()
            && migratedReopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::LegacyV1
            && talas::project::serializeProjectDocument(
                   migratedReopened.document)
                == migratedCanonical,
        "migrated legacy dialect should save and reopen byte-stably");

    auto premature = canonical;
    const auto currentVersion = premature.find(
        R"json("version": 156)json");
    premature.replace(
        currentVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  22)json");
    requireLoadErrorAt(
        premature,
        "$.expressionLanguage",
        "format 22 should reject premature expression-language authority");

    talas::project::ProjectDocument pureV2Source;
    pureV2Source.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV2;
    pureV2Source.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {expressionParameter("value", "[1, 2][1]")},
    });
    talas::project::reconcileProjectPresentation(pureV2Source);
    auto formatTwentyThree =
        talas::project::serializeProjectDocument(pureV2Source);
    const auto v25 = formatTwentyThree.find(R"json("version": 156)json");
    const auto v2Authority = formatTwentyThree.find(
        R"json("expressionLanguage": "pureV2")json");
    require(v25 != std::string::npos && v2Authority != std::string::npos,
        "format-25 fixture should expose version and language authority");
    formatTwentyThree.replace(
        v25,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  23)json");
    eraseEmptyExpressionDefinitions(formatTwentyThree);
    const auto migratedV23 =
        talas::project::parseProjectDocument(formatTwentyThree);
    require(
        migratedV23.ok()
            && migratedV23.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedV23.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV2,
        "format 23 should migrate while retaining exact PureV2 authority");
    requireContains(
        talas::project::serializeProjectDocument(migratedV23.document),
        R"json("expressionLanguage": "pureV2")json",
        "migrated format 23 should remain PureV2 in canonical format 26");

    auto prematureV3 = formatTwentyThree;
    const auto v2 = prematureV3.find(
        R"json("expressionLanguage": "pureV2")json");
    prematureV3.replace(
        v2,
        std::string_view{R"json("expressionLanguage": "pureV2")json"}.size(),
        R"json("expressionLanguage": "pureV3")json");
    requireLoadErrorAt(
        prematureV3,
        "$.expressionLanguage",
        "format 23 should reject premature PureV3 authority");

    talas::project::ProjectDocument pureV3Source;
    pureV3Source.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV3;
    pureV3Source.graph.nodes.push_back({
        "series",
        "talas.constant",
        {expressionParameter("value", "series(1, 2, 3)[1]")},
    });
    talas::project::reconcileProjectPresentation(pureV3Source);
    auto formatTwentyFour =
        talas::project::serializeProjectDocument(pureV3Source);
    const auto format25 = formatTwentyFour.find(R"json("version": 156)json");
    require(format25 != std::string::npos,
        "format-25 fixture should expose the current version");
    formatTwentyFour.replace(
        format25,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  24)json");
    eraseEmptyExpressionDefinitions(formatTwentyFour);
    const auto migratedV24 =
        talas::project::parseProjectDocument(formatTwentyFour);
    require(
        migratedV24.ok()
            && migratedV24.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedV24.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV3
            && migratedV24.document.expressionDefinitions.definitions.empty(),
        "format 24 should migrate losslessly with empty definitions and exact PureV3 authority");

    auto prematureV4 = formatTwentyFour;
    const auto v3 = prematureV4.find(
        R"json("expressionLanguage": "pureV3")json");
    prematureV4.replace(
        v3,
        std::string_view{R"json("expressionLanguage": "pureV3")json"}.size(),
        R"json("expressionLanguage": "pureV4")json");
    requireLoadErrorAt(
        prematureV4,
        "$.expressionLanguage",
        "format 24 should reject premature PureV4 authority");

    talas::project::ProjectDocument pureV4Source = pure;
    pureV4Source.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV4;
    auto formatTwentyFive =
        talas::project::serializeProjectDocument(pureV4Source);
    const auto current26 = formatTwentyFive.find(
        R"json("version": 156)json");
    require(current26 != std::string::npos,
        "format-25 migration fixture should begin at the current version");
    formatTwentyFive.replace(
        current26,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  25)json");
    eraseVariableLibrary(formatTwentyFive);
    const auto migratedV25 =
        talas::project::parseProjectDocument(formatTwentyFive);
    require(
        migratedV25.ok()
            && migratedV25.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedV25.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV4
            && migratedV25.document.expressionDefinitions
                == pureV4Source.expressionDefinitions,
        "format 25 should migrate losslessly with exact PureV4 authority");

    auto prematureSession = formatTwentyFive;
    const auto pureV4Authority = prematureSession.find(
        R"json("expressionLanguage": "pureV4")json");
    require(pureV4Authority != std::string::npos,
        "format-25 fixture should expose PureV4 authority");
    prematureSession.replace(
        pureV4Authority,
        std::string_view{
            R"json("expressionLanguage": "pureV4")json"}.size(),
        R"json("expressionLanguage": "sessionV1")json");
    requireLoadErrorAt(
        prematureSession,
        "$.expressionLanguage",
        "format 25 should reject premature SessionV1 authority");

    talas::project::ProjectDocument stateful;
    stateful.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::SessionV1;
    stateful.graph.nodes.push_back({
        "stateful",
        "talas.constant",
        {expressionParameter("value", "previous(!Input, 0)")},
    });
    talas::project::reconcileProjectPresentation(stateful);
    auto statefulCanonical =
        talas::project::serializeProjectDocument(stateful);
    const auto statefulVersion = statefulCanonical.find(
        R"json("version": 156)json");
    require(statefulVersion != std::string::npos,
        "stateful migration fixture should expose current version");
    statefulCanonical.replace(
        statefulVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  26)json");
    eraseVariableLibrary(statefulCanonical);
    const auto statefulReopened =
        talas::project::parseProjectDocument(statefulCanonical);
    require(
        statefulReopened.ok()
            && statefulReopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && statefulReopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::SessionV1
            && statefulReopened.document.graph.nodes.front()
                    .parameters.front().expression
                == stateful.graph.nodes.front().parameters.front().expression,
        "format 26 should migrate live stateful expression bytes exactly into format 29");
}

void testFormatTwelveAddsEmptySampleMapLibrary()
{
    const auto serialized = talas::project::serializeProjectDocument(
        talas::project::ProjectDocument{});

    requireContains(
        serialized,
        R"json("version": 156)json",
        "current project serialization should use format twelve");
    requireContains(
        serialized,
        R"json("sampleMapLibrary": {
    "version": 1,
    "maps": []
  })json",
        "format twelve should carry a required empty sample-map library");

    const auto loaded = talas::project::parseProjectDocument(serialized);
    require(
        loaded.ok(),
        "empty format-twelve project should parse"
            + (loaded.errors.empty()
                   ? std::string{}
                   : ": " + loaded.errors.front().path + " "
                       + loaded.errors.front().message));
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
        "parsed current project should retain format-twelve identity");
}

void testFormatThirteenPreservedComponentRoundTripsCanonically()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode node;
    node.instanceId = "missing1";
    node.typeId = "org.example.spectral-colour";
    node.parameters.push_back(choiceParameter("mode", "colour"));

    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Spectral Colour";
    component.bundleId = "org.example.spectral-colour";
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
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    node.preservedComponent = component;
    document.graph.nodes.push_back(std::move(node));
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "missing1")
        ->position = {120, 80};

    const auto serialized =
        talas::project::serializeProjectDocument(document);
    requireContains(
        serialized,
        R"json("version": 156)json",
        "preserved component writer should use format thirteen");
    requireContains(
        serialized,
        R"json("stateEncoding": "base64",
          "state": "AP9B")json",
        "preserved component should write canonical opaque bytes");
    requireContains(
        serialized,
        R"json("spectralFormat": "complexSpectrumV1")json",
        "preserved component should write the complete frozen signal contract");
    requireContains(
        serialized,
        R"json("choices": ["clean", "colour"])json",
        "preserved component should write ordered parameter choices");

    const auto loaded = talas::project::parseProjectDocument(serialized);
    require(loaded.ok(), "canonical format-thirteen component should parse");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
        "parsed preserved component should retain current format identity");
    require(
        loaded.document.graph.nodes.size() == 1
            && loaded.document.graph.nodes.front().preservedComponent
                == std::optional{component},
        "preserved component interface and arbitrary bytes should round-trip");
    require(
        talas::project::serializeProjectDocument(loaded.document)
            == serialized,
        "preserved component save/reopen should remain canonical");

    auto malformedState = serialized;
    const auto state = malformedState.find(R"json("state": "AP9B")json");
    require(state != std::string::npos, "fixture should contain opaque state");
    malformedState.replace(
        state,
        std::string_view{R"json("state": "AP9B")json"}.size(),
        R"json("state": "AP-B")json");
    requireLoadErrorAt(
        malformedState,
        "$.graph.nodes[0].preservedComponent.state",
        "non-canonical component state should fail at its exact path");

    const auto requireRejectedReplacement = [&serialized](
                                                std::string_view field,
                                                std::string_view replacement,
                                                std::string_view path,
                                                const std::string& message) {
        auto malformed = serialized;
        const auto position = malformed.find(field);
        require(position != std::string::npos, "capsule fixture field should exist");
        malformed.replace(position, field.size(), replacement);
        requireLoadErrorAt(malformed, path, message);
    };
    requireRejectedReplacement(
        R"json("schemaVersion": 1)json",
        R"json("schemaVersion": 2)json",
        "$.graph.nodes[0].preservedComponent.schemaVersion",
        "future capsule schemas should fail at the schema field");
    requireRejectedReplacement(
        R"json("typeVersion": "1.2.3")json",
        R"json("typeVersion": "01.2.3")json",
        "$.graph.nodes[0].preservedComponent.typeVersion",
        "malformed semantic versions should fail at the version field");
    requireRejectedReplacement(
        R"json("stateSchemaVersion": 4)json",
        R"json("stateSchemaVersion": 0)json",
        "$.graph.nodes[0].preservedComponent.stateSchemaVersion",
        "zero component state schema should fail at the state version field");
    requireRejectedReplacement(
        R"json("stateEncoding": "base64")json",
        R"json("stateEncoding": "executable")json",
        "$.graph.nodes[0].preservedComponent.stateEncoding",
        "future state encodings should fail closed");

    auto futureFieldInLegacy = serialized;
    const auto currentVersion =
        futureFieldInLegacy.find(R"json("version": 156)json");
    futureFieldInLegacy.replace(
        currentVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  12)json");
    eraseFormatSeventeenTransportSync(futureFieldInLegacy);
    eraseFormatEighteenOscIngress(futureFieldInLegacy);
    eraseFormatSixteenMusicalTime(futureFieldInLegacy);
    requireLoadErrorAt(
        futureFieldInLegacy,
        "$.graph.nodes[0].preservedComponent",
        "format twelve should reject future preservation capsules");

    talas::project::ProjectDocument nestedDocument;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    auto nestedNode = loaded.document.graph.nodes.front();
    nestedNode.instanceId = "nestedMissing";
    definition.graph.nodes.push_back(std::move(nestedNode));
    nestedDocument.soundDefinitions.push_back(std::move(definition));
    talas::project::reconcileProjectPresentation(nestedDocument);
    const auto nestedJson =
        talas::project::serializeProjectDocument(nestedDocument);
    const auto nestedLoaded =
        talas::project::parseProjectDocument(nestedJson);
    require(
        nestedLoaded.ok()
            && nestedLoaded.document.soundDefinitions.front()
                   .graph.nodes.front().preservedComponent
                == std::optional{component},
        "definition-local component capsules should round-trip in scope");

    talas::project::ProjectDocument soundInstanceDocument;
    talas::project::ProjectSoundDefinition referencedDefinition;
    referencedDefinition.definitionId = "voice";
    referencedDefinition.displayName = "Voice";
    soundInstanceDocument.soundDefinitions.push_back(
        std::move(referencedDefinition));
    talas::project::ProjectNode soundInstance;
    soundInstance.instanceId = "voice1";
    soundInstance.soundDefinitionId = "voice";
    soundInstance.preservedComponent = component;
    soundInstanceDocument.graph.nodes.push_back(
        std::move(soundInstance));
    talas::project::reconcileProjectPresentation(soundInstanceDocument);
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(soundInstanceDocument),
        "$.graph.nodes[0].preservedComponent",
        "reusable Sound instances should not carry component capsules");
}

void testFormatTwelveMigratesWithoutPreservedComponents()
{
    const talas::project::ProjectDocument empty;
    auto legacy = talas::project::serializeProjectDocument(empty);
    const auto version = legacy.find(R"json("version": 156)json");
    require(version != std::string::npos, "current fixture should use format thirteen");
    legacy.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  12)json");
    eraseFormatSeventeenTransportSync(legacy);
    eraseFormatEighteenOscIngress(legacy);
    eraseFormatSixteenMusicalTime(legacy);
    eraseExpressionLanguage(legacy);
    downgradePresentationToV1(legacy, empty);

    const auto loaded = talas::project::parseProjectDocument(legacy);
    require(loaded.ok(), "format twelve should migrate without filesystem access");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
        "format twelve should migrate to the current format in memory");
    require(
        std::ranges::none_of(
            loaded.document.graph.nodes,
            [](const auto& node) {
                return node.preservedComponent.has_value();
            }),
        "format-twelve migration should not invent preservation capsules");
}

void testFormatElevenPopulatedSnapshotBankRoundTrips()
{
    auto source = talas::project::serializeProjectDocument({});
    const auto emptyBanks = source.find(R"json("banks": [])json");
    require(
        emptyBanks != std::string::npos,
        "format-eleven fixture should expose the reserved snapshot bank array");
    source.replace(
        emptyBanks,
        std::string_view{R"json("banks": [])json"}.size(),
        R"json("banks": [
      {
        "id": "project-main",
        "name": "Project",
        "scope": {
          "kind": "project"
        },
        "smoothingSeconds": 0.25,
        "parameterPolicies": [],
        "snapshots": [
          {
            "id": "init",
            "name": "Init",
            "values": []
          }
        ]
      }
    ])json");

    const auto loaded = talas::project::parseProjectDocument(source);
    require(
        loaded.ok(),
        "format eleven should load a populated version-one snapshot bank");

    const auto canonical =
        talas::project::serializeProjectDocument(loaded.document);
    requireContains(
        canonical,
        R"json("id": "project-main")json",
        "canonical snapshot output should preserve the stable bank identity");
    requireContains(
        canonical,
        R"json("id": "init")json",
        "canonical snapshot output should preserve the stable snapshot identity");
    require(
        talas::project::serializeProjectDocument(
            talas::project::parseProjectDocument(canonical).document)
            == canonical,
        "populated snapshot serialization should be byte-stable");
}

void testAllSnapshotScopesTargetsAndValuesRoundTripCanonically()
{
    talas::project::ProjectDocument document;

    talas::project::SnapshotTargetIdentity eventTarget;
    eventTarget.kind = talas::project::SnapshotTargetKind::EventValue;
    eventTarget.eventName = "Gain";
    eventTarget.domain = talas::expr::ValueDomain::Scalar;

    talas::project::SnapshotTargetIdentity nodeTarget;
    nodeTarget.kind = talas::project::SnapshotTargetKind::NodeParameter;
    nodeTarget.nodeId = "osc-main";
    nodeTarget.parameterId = "frequency";

    talas::project::SnapshotTargetIdentity soundTarget;
    soundTarget.kind = talas::project::SnapshotTargetKind::SoundParameter;
    soundTarget.instancePath = {"voice-a"};
    soundTarget.definitionId = "voice-def";
    soundTarget.parameterId = "enabled";

    talas::project::ProjectSnapshot projectSnapshot;
    projectSnapshot.snapshotId = "snapshot-a";
    projectSnapshot.displayName = "Snapshot A";
    projectSnapshot.values = {
        {eventTarget,
         {talas::project::SnapshotValueKind::Number,
          talas::expr::ValueDomain::Scalar,
          0.75}},
        {nodeTarget,
         {talas::project::SnapshotValueKind::Choice,
          talas::expr::ValueDomain::Scalar,
          0.0,
          "triangle"}},
        {soundTarget,
         {talas::project::SnapshotValueKind::Boolean,
          talas::expr::ValueDomain::Scalar,
          0.0,
          {},
          true}},
    };

    talas::project::ProjectSnapshotBank projectBank;
    projectBank.bankId = "project-bank";
    projectBank.displayName = "Project Bank";
    projectBank.smoothingSeconds = 0.125;
    projectBank.parameterPolicies = {
        {eventTarget, true, talas::project::SnapshotParameterRange{0.0, 1.0}},
        {nodeTarget, false, std::nullopt},
    };
    projectBank.snapshots = {projectSnapshot};

    talas::project::ProjectSnapshotBank definitionBank;
    definitionBank.bankId = "definition-bank";
    definitionBank.displayName = "Definition Bank";
    definitionBank.scope.kind = talas::project::SnapshotScopeKind::Definition;
    definitionBank.scope.instancePath = {"voice-a"};
    definitionBank.scope.definitionId = "voice-def";

    talas::project::ProjectSnapshotBank nodeBank;
    nodeBank.bankId = "node-bank";
    nodeBank.displayName = "Node Bank";
    nodeBank.scope.kind = talas::project::SnapshotScopeKind::Node;
    nodeBank.scope.instancePath = {"voice-a"};
    nodeBank.scope.containingDefinitionId = "voice-def";
    nodeBank.scope.nodeId = "filter-a";

    document.snapshotLibrary.banks = {
        projectBank,
        definitionBank,
        nodeBank,
    };

    const auto canonical =
        talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(canonical);
    require(
        loaded.ok(),
        "all snapshot scope, target, policy, and typed-value variants should load");
    require(
        loaded.document.snapshotLibrary == document.snapshotLibrary,
        "snapshot variants should round-trip without semantic target rebinding");
    require(
        talas::project::serializeProjectDocument(loaded.document) == canonical,
        "all populated snapshot variants should serialize byte-stably");

    auto duplicateTarget = document;
    duplicateTarget.snapshotLibrary.banks.front()
        .snapshots.front()
        .values.push_back(
            duplicateTarget.snapshotLibrary.banks.front()
                .snapshots.front()
                .values.front());
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(duplicateTarget),
        "$.snapshotLibrary.banks[0].snapshots[0].values[3].target",
        "duplicate snapshot targets should be rejected without ordinal rebinding");
}

talas::project::ProjectDocument makePatchDocument()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Roundtrip Patch";
    document.graph.nodes.push_back({
        "osc1",
        "talas.oscillator",
        {{"frequency", "220 * 2"}}
    });
    document.graph.nodes.push_back({
        "vca1",
        "talas.vca",
        {{"gain", "0.75"}}
    });
    document.graph.nodes.push_back({"out1", "talas.audioOutput", {}});
    document.graph.connections.push_back({"osc1", "audioOut", "vca1", "audioIn"});
    document.graph.connections.push_back({"vca1", "audioOut", "out1", "audioIn"});
    return document;
}

void testGraphRoundTripsThroughJson()
{
    const auto document = makePatchDocument();

    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(document));

    require(loaded.ok(), "serialized project should parse without errors");
    require(loaded.document.metadata.title == "Roundtrip Patch", "metadata title should round-trip");
    require(loaded.document.graph.nodes.size() == 3, "node count should round-trip");
    require(loaded.document.graph.nodes[0].instanceId == "osc1", "node id order should round-trip");
    require(loaded.document.graph.nodes[0].typeId == "talas.oscillator", "node type id should round-trip");
    require(loaded.document.graph.nodes[0].parameters.size() == 1, "node parameters should round-trip");
    require(loaded.document.graph.nodes[0].parameters[0].parameterId == "frequency", "parameter id should round-trip");
    require(loaded.document.graph.nodes[0].parameters[0].expression.has_value(), "parameter expression should round-trip as present");
    require(*loaded.document.graph.nodes[0].parameters[0].expression == "220 * 2", "parameter expression should round-trip");
    require(loaded.document.graph.connections.size() == 2, "connection count should round-trip");
    require(loaded.document.graph.connections[1].fromNode == "vca1", "connection order should round-trip");
    require(loaded.document.graph.connections[1].toPort == "audioIn", "connection endpoint should round-trip");
}

void testHistoricalPackedFftJsonIdentityRoundTripsUnchanged()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "fft",
        "talas.fft",
        {
            {"length", "64"},
            booleanParameter("inverse", true),
        },
    });
    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("typeId": "talas.fft")json",
                    "historical FFT type ID should remain serialized unchanged");
    requireContains(json, R"json("id": "length")json",
                    "historical FFT length parameter ID should remain unchanged");
    requireContains(json, R"json("expression": "64")json",
                    "historical FFT length expression should remain unchanged");
    requireContains(json, R"json("id": "inverse")json",
                    "historical FFT inverse parameter ID should remain unchanged");
    requireContains(json, R"json("boolean": true)json",
                    "historical FFT inverse boolean should remain unchanged");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok()
                && loaded.document.graph.nodes.size() == 1
                && loaded.document.graph.nodes.front().typeId == "talas.fft"
                && loaded.document.graph.nodes.front().parameters.size() == 2,
            "historical FFT JSON should parse with the same descriptor fields");
    require(talas::project::serializeProjectDocument(loaded.document) == json,
            "historical FFT JSON should remain byte-canonical after round-trip");
}

void testSoundDefinitionsRoundTripThroughCurrentJson()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.inputs.push_back({
        "frequency",
        talas::core::PortType::Control,
        "osc",
        "frequency",
    });
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {{"frequency", "220"}},
    });
    document.soundDefinitions.push_back(definition);
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        "voice",
        "osc")->position = {120, 80};

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("version": 156)json", "nested Sound projects should serialize as the current version");
    requireContains(json, R"json("soundDefinitions": [)json", "nested Sound projects should serialize definitions");
    requireContains(json, R"json("type": "control")json", "nested Sound inputs should serialize their type");
    requireContains(json, R"json("nodePortId": "audioOut")json", "nested Sound boundaries should serialize internal endpoints");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "serialized nested Sound project should parse");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion, "nested Sound project should load as the current version");
    require(loaded.document.soundDefinitions.size() == 1, "nested Sound definition count should round-trip");
    const auto& loadedDefinition = loaded.document.soundDefinitions.front();
    require(loadedDefinition.definitionId == "voice", "nested Sound identity should round-trip");
    require(loadedDefinition.displayName == "Voice", "nested Sound display name should round-trip");
    require(loadedDefinition.inputs.size() == 1, "nested Sound inputs should round-trip");
    require(
        loadedDefinition.inputs.front().type == talas::core::PortType::Control,
        "nested Sound input type should round-trip");
    require(
        loadedDefinition.outputs.front().nodePortId == "audioOut",
        "nested Sound output endpoint should round-trip");
    require(loadedDefinition.graph.nodes.size() == 1, "nested Sound graph should round-trip");
    require(
        nodePosition(loaded.document, "osc", "voice").x == 120,
        "nested Sound node position should round-trip");
    require(
        talas::project::serializeProjectDocument(loaded.document) == json,
        "nested Sound JSON should be canonical after round-trip");
    require(
        talas::project::makeGraph(loaded.document).nodes().empty(),
        "persisted Sound definitions should not alter root graph execution");
}

void testSoundInterfacesAndSparseOverridesRoundTripThroughVersionNineJson()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.interfaceVersion = 3;
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {},
        {},
    });

    talas::project::ProjectSoundExposedParameter frequency;
    frequency.parameterId = "frequency";
    frequency.displayName = "Frequency";
    frequency.kind = talas::core::ParameterKind::Expression;
    frequency.domain = talas::expr::ValueDomain::Hertz;
    frequency.defaultValue.parameterId = "frequency";
    frequency.defaultValue.expression = "440 hz";
    frequency.aliases = {"pitchHz"};
    frequency.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "osc",
        "frequency",
        {},
        talas::expr::ValueDomain::Hertz,
    });
    definition.exposedParameters.push_back(std::move(frequency));
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::ProjectNode instance;
    instance.instanceId = "voiceA";
    instance.soundDefinitionId = "voice";
    instance.parameters.push_back({"frequency", "220 hz"});
    document.graph.nodes.push_back(std::move(instance));

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("version": 156)json",
                    "Sound interfaces should serialize as format twelve");
    requireContains(json, R"json("interfaceVersion": 3)json",
                    "Sound interface version should serialize");
    requireContains(json, R"json("target": "nodeParameter")json",
                    "Sound parameter binding target should serialize");
    requireContains(json, R"json("aliases": [)json",
                    "Sound parameter aliases should serialize canonically");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "format-twelve Sound interface should parse");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
            "format-ten Sound interface source should load as current format twelve");
    const auto& loadedDefinition = loaded.document.soundDefinitions.front();
    require(
        loadedDefinition.interfaceVersion == 3
            && loadedDefinition.exposedParameters.size() == 1
            && loadedDefinition.exposedParameters.front().parameterId
                == "frequency"
            && loadedDefinition.exposedParameters.front().aliases
                == std::vector<std::string>{"pitchHz"}
            && loadedDefinition.exposedParameters.front().bindings.size()
                == 1,
        "Sound interface identity, alias, and binding should round-trip");
    require(
        loaded.document.graph.nodes.front().parameters.size() == 1
            && loaded.document.graph.nodes.front().parameters.front()
                .expression
                == std::optional<std::string>{"220 hz"},
        "sparse Sound-instance override should round-trip");
    require(
        talas::project::serializeProjectDocument(loaded.document) == json,
        "format-ten Sound interface JSON should be canonical");

    auto aliasDocument = document;
    aliasDocument.graph.nodes.front().parameters.front().parameterId =
        "pitchHz";
    const auto migratedAlias = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(aliasDocument));
    require(
        migratedAlias.ok()
            && migratedAlias.document.graph.nodes.front()
                .parameters.front().parameterId
                == "frequency",
        "an explicit compatibility alias should migrate to the stable exposed parameter id on load");

    auto renamedDocument = document;
    renamedDocument.soundDefinitions.front().exposedParameters.front()
        .displayName = "Pitch";
    const auto renamed = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(renamedDocument));
    require(
        renamed.ok()
            && renamed.document.soundDefinitions.front()
                   .exposedParameters.front().displayName
                == "Pitch"
            && renamed.document.soundDefinitions.front()
                   .exposedParameters.front().parameterId
                == "frequency"
            && renamed.document.graph.nodes.front().parameters.front()
                   .parameterId
                == "frequency",
        "display-only Sound parameter rename should preserve executable identity and overrides");

    auto ambiguousOverride = document;
    ambiguousOverride.graph.nodes.front().parameters.push_back(
        {"pitchHz", "330 hz"});
    require(
        !talas::project::parseProjectDocument(
             talas::project::serializeProjectDocument(
                 ambiguousOverride))
             .ok(),
        "canonical and alias overrides on one instance should fail alias migration instead of rebinding silently");

    auto unknownOverride = document;
    unknownOverride.graph.nodes.front().parameters.front().parameterId =
        "removedParameter";
    const auto recoverableUnknown = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(unknownOverride));
    require(
        recoverableUnknown.ok()
            && recoverableUnknown.document.graph.nodes.front()
                   .parameters.front().parameterId
                == "removedParameter",
        "unknown Sound overrides should remain recoverable data until an explicit alias resolves them");
}

void testEverySoundInterfaceParameterKindRoundTripsCanonically()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "typedVoice";
    definition.displayName = "Typed Voice";
    definition.interfaceVersion = 2;
    definition.graph.nodes = {
        {"osc", "talas.oscillator", {}, {}},
        {"sample", "talas.sample", {}, {}},
    };

    const auto addParameter = [&](
                                  std::string id,
                                  std::string name,
                                  talas::core::ParameterKind kind,
                                  talas::project::ProjectParameterExpression defaultValue,
                                  std::vector<std::string> choices,
                                  std::string nodeId,
                                  std::string parameterId) {
        talas::project::ProjectSoundExposedParameter exposed;
        exposed.parameterId = std::move(id);
        exposed.displayName = std::move(name);
        exposed.kind = kind;
        exposed.choices = std::move(choices);
        exposed.defaultValue = std::move(defaultValue);
        exposed.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            std::move(nodeId),
            std::move(parameterId),
            {},
            talas::expr::ValueDomain::Scalar,
        });
        definition.exposedParameters.push_back(std::move(exposed));
    };
    addParameter(
        "interpolation",
        "Interpolation",
        talas::core::ParameterKind::Choice,
        choiceParameter("interpolation", "linear"),
        {"linear", "none"},
        "osc",
        "interpolation");
    addParameter(
        "loop",
        "Loop",
        talas::core::ParameterKind::Boolean,
        booleanParameter("loop", false),
        {},
        "sample",
        "setLoop");
    addParameter(
        "sampleAsset",
        "Sample",
        talas::core::ParameterKind::Resource,
        resourceParameter("sampleAsset", "Samples/Default"),
        {},
        "sample",
        "sample");
    addParameter(
        "modulation",
        "Modulation",
        talas::core::ParameterKind::Text,
        textParameter("modulation", "phase"),
        {},
        "osc",
        "modulation");
    document.soundDefinitions.push_back(std::move(definition));

    auto instance = talas::project::ProjectNode{};
    instance.instanceId = "typedVoice1";
    instance.soundDefinitionId = "typedVoice";
    instance.parameters = {
        choiceParameter("interpolation", "none"),
        booleanParameter("loop", true),
        resourceParameter("sampleAsset", "Samples/Override"),
        textParameter("modulation", "frequency"),
    };
    document.graph.nodes.push_back(std::move(instance));

    const auto json = talas::project::serializeProjectDocument(document);
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(),
            "every non-expression Sound interface kind should parse");
    require(
        loaded.document.soundDefinitions.front().exposedParameters
            == document.soundDefinitions.front().exposedParameters,
        "Choice, Boolean, Resource, and Text interface descriptors should round-trip exactly");
    require(
        loaded.document.graph.nodes.front().parameters
            == document.graph.nodes.front().parameters,
        "typed sparse Sound overrides should round-trip exactly");
    require(
        talas::project::serializeProjectDocument(loaded.document) == json,
        "all typed Sound interface JSON should remain canonical");
}

void testVersionNineSoundInterfaceParsingRejectsInvalidSchema()
{
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice",
      "inputs": [], "outputs": [], "parameters": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].interfaceVersion",
        "format-nine Sound interface version should be required");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 0,
      "inputs": [], "outputs": [], "parameters": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].interfaceVersion",
        "format-nine Sound interface version should be positive");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 1,
      "inputs": [], "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].parameters",
        "format-nine Sound parameters array should be required");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 1,
      "inputs": [], "outputs": [],
      "parameters": [{
        "id": "mode", "name": "Mode", "kind": "choice",
        "domain": "scalar", "choices": ["linear", "none"],
        "default": { "expression": "1" }, "aliases": [],
        "bindings": [{
          "target": "nodeParameter", "nodeId": "osc",
          "parameterId": "interpolation"
        }]
      }],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].parameters[0].default",
        "Sound parameter default should match its declared kind");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 1,
      "inputs": [], "outputs": [],
      "parameters": [{
        "id": "mode", "name": "Mode", "kind": "choice",
        "domain": "scalar", "choices": ["linear"],
        "default": { "choice": "linear" }, "aliases": [],
        "bindings": []
      }],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].parameters[0].bindings",
        "Sound parameter should require an explicit binding");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 1,
      "inputs": [], "outputs": [],
      "parameters": [{
        "id": "frequency", "name": "Frequency", "kind": "expression",
        "domain": "hertz", "choices": [],
        "default": { "expression": "440 hz" },
        "aliases": ["frequency"],
        "bindings": [{
          "target": "nodeParameter", "nodeId": "osc",
          "parameterId": "frequency"
        }]
      }],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].parameters[0].aliases",
        "Sound parameter alias should not collide with its canonical id");
    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "voice", "name": "Voice", "interfaceVersion": 1,
      "inputs": [], "outputs": [],
      "parameters": [{
        "id": "frequency", "name": "Frequency", "kind": "expression",
        "domain": "hertz", "choices": [],
        "default": { "expression": "440 hz" }, "aliases": [],
        "bindings": [{ "target": "nodeParameter", "nodeId": "osc" }]
      }],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].parameters[0].bindings[0].parameterId",
        "node parameter binding should require its target parameter id");
}

void testVersionEightSoundDefinitionsMigrateToInterfaceVersionOne()
{
    const auto loaded = talas::project::parseProjectDocument(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"));
    require(loaded.ok(), "format-eight Sound definition should migrate");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion
            && loaded.document.soundDefinitions.front().interfaceVersion
                == 1
            && loaded.document.soundDefinitions.front().exposedParameters
                .empty(),
        "format-eight Sound definitions should migrate to an empty version-one interface");
    const auto migrated = talas::project::serializeProjectDocument(
        loaded.document);
    requireContains(migrated, R"json("version": 156)json",
                    "migrated Sound definition should save as format twelve");
    requireContains(migrated, R"json("interfaceVersion": 1)json",
                    "migrated Sound interface version should serialize");
}

void testSoundInstancesRoundTripWithExplicitDefinitionIdentity()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    document.graph.nodes.push_back(instance);
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "voice1")->position = {48, 96};

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(
        json,
        R"json("soundDefinitionId": "voice")json",
        "Sound instances should serialize their explicit definition identity");
    require(
        json.find(R"json("typeId": "")json") == std::string::npos,
        "Sound instances should not serialize an empty built-in type identity");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "serialized Sound instance should parse");
    require(
        loaded.document.graph.nodes.size() == 1
            && loaded.document.graph.nodes.front().typeId.empty()
            && loaded.document.graph.nodes.front().soundDefinitionId
                == std::optional<std::string>{"voice"},
        "Sound instance identity should round-trip independently from built-in type identity");
    require(
        talas::project::serializeProjectDocument(loaded.document) == json,
        "Sound instance JSON should remain canonical after round-trip");
}

void testNodeParsingRequiresExactlyOneTypeIdentity()
{
    auto document = talas::project::serializeProjectDocument(
        talas::project::ProjectDocument{});
    const auto emptyNodes = std::string{"\"nodes\": []"};
    const auto nodePosition = document.rfind(emptyNodes);
    require(nodePosition != std::string::npos, "empty root graph should contain a nodes array");

    auto missingIdentity = document;
    missingIdentity.replace(
        nodePosition,
        emptyNodes.size(),
        R"json("nodes": [
      {
        "id": "voice1",
        "position": {
          "x": 0,
          "y": 0
        },
        "parameters": []
      }
    ])json");
    requireLoadErrorAt(
        missingIdentity,
        "$.graph.nodes[0]",
        "node without a built-in or Sound identity should fail");

    auto ambiguousIdentity = document;
    ambiguousIdentity.replace(
        nodePosition,
        emptyNodes.size(),
        R"json("nodes": [
      {
        "id": "voice1",
        "typeId": "talas.oscillator",
        "soundDefinitionId": "voice",
        "position": {
          "x": 0,
          "y": 0
        },
        "parameters": []
      }
    ])json");
    requireLoadErrorAt(
        ambiguousIdentity,
        "$.graph.nodes[0]",
        "node with both built-in and Sound identities should fail");
}

void testEverySoundBoundaryPortTypeRoundTrips()
{
    const std::vector<talas::core::PortType> portTypes = {
        talas::core::PortType::Audio,
        talas::core::PortType::Control,
        talas::core::PortType::Event,
        talas::core::PortType::SampleBuffer,
        talas::core::PortType::Wavetable,
        talas::core::PortType::Spectrum,
        talas::core::PortType::ComplexSpectrum,
        talas::core::PortType::NoteStream,
    };

    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "allTypes";
    definition.displayName = "All Types";
    for (std::size_t index = 0; index < portTypes.size(); ++index)
    {
        definition.inputs.push_back({
            "port" + std::to_string(index),
            portTypes[index],
            "node",
            "input",
        });
    }
    document.soundDefinitions.push_back(std::move(definition));

    const auto loaded = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(loaded.ok(), "every nested Sound boundary type should parse");
    require(
        loaded.document.soundDefinitions.front().inputs.size()
            == portTypes.size(),
        "every nested Sound boundary type should survive serialization");
    for (std::size_t index = 0; index < portTypes.size(); ++index)
    {
        require(
            loaded.document.soundDefinitions.front().inputs[index].type
                == portTypes[index],
            "nested Sound boundary port type should round-trip");
    }
}

void testComplexSpectrumSoundBoundaryUsesFormatTen()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "futureSpectrum";
    definition.displayName = "Future Spectrum";
    definition.inputs.push_back({
        "frames",
        talas::core::PortType::ComplexSpectrum,
        "node",
        "frames",
    });
    document.soundDefinitions.push_back(std::move(definition));

    const auto current =
        talas::project::serializeProjectDocument(document);
    require(
        current.find(R"json("version": 156)json")
                != std::string::npos
            && current.find(R"json("type": "complexSpectrum")json")
                != std::string::npos,
        "format-ten serialization should persist ComplexSpectrum boundaries");
    const auto roundTrip =
        talas::project::parseProjectDocument(current);
    require(
        roundTrip.ok()
            && roundTrip.document.soundDefinitions.front().inputs.front().type
                == talas::core::PortType::ComplexSpectrum,
        "format-ten ComplexSpectrum boundaries should round-trip");

    requireLoadErrorAt(
        versionNineProjectWithSoundDefinitions(R"json([
    {
      "id": "futureSpectrum",
      "name": "Future Spectrum",
      "inputs": [
        { "id": "frames", "type": "complexSpectrum", "nodeId": "node", "nodePortId": "frames" }
      ],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].inputs[0].type",
        "format-nine parsing must reject future ComplexSpectrum boundaries");
}

void testVersionSevenMigratesToEmptyCurrentSoundDefinitions()
{
    talas::project::ProjectDocument current;
    current.timeline.eventValues.push_back({
        0.5,
        "Gain",
        talas::expr::ValueDomain::Scalar,
        0.75,
        talas::core::AutomationInterpolation::Linear,
    });
    auto json = talas::project::serializeProjectDocument(current);
    const auto versionPosition = json.find(R"json("version": 156)json");
    require(versionPosition != std::string::npos, "current serializer should emit version eleven");
    json.replace(versionPosition, std::string{R"json("version": 156)json"}.size(), R"json("version":   7)json");
    eraseFormatSeventeenTransportSync(json);
    eraseFormatEighteenOscIngress(json);
    eraseFormatSixteenMusicalTime(json);
    eraseExpressionLanguage(json);
    const auto definitions = std::string{"  \"soundDefinitions\": [],\n"};
    const auto definitionsPosition = json.find(definitions);
    require(definitionsPosition != std::string::npos, "current serializer should emit Sound definitions");
    json.erase(definitionsPosition, definitions.size());
    const auto stateStart = json.find("  \"presentation\": {");
    const auto controlsStart = json.find("  \"virtualControls\": ");
    require(
        stateStart != std::string::npos
            && controlsStart != std::string::npos
            && stateStart < controlsStart,
        "current serializer should emit removable format-eleven state collections");
    json.erase(stateStart, controlsStart - stateStart);

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "version-seven project should migrate without Sound definitions");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion, "version-seven project should migrate to the current version");
    require(loaded.document.soundDefinitions.empty(), "version-seven migration should create no Sound definitions");
    require(
        loaded.document.timeline.eventValues.size() == 1
            && loaded.document.timeline.eventValues.front().interpolation
                == talas::core::AutomationInterpolation::Linear,
        "version-seven migration should preserve linear Timeline interpolation");
}

void testSoundDefinitionParsingRejectsInvalidModels()
{
    requireLoadErrorAt(
        projectWithSoundDefinitions(R"json([
    {
      "id": "bad id",
      "name": "Bad",
      "inputs": [],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].id",
        "Sound definition IDs should be identifiers");

    requireLoadErrorAt(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    },
    {
      "id": "voice",
      "name": "Duplicate",
      "inputs": [],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[1].id",
        "Sound definition IDs should be unique");

    requireLoadErrorAt(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [
        { "id": "frequency", "type": "control", "nodeId": "osc", "nodePortId": "frequency" },
        { "id": "frequency", "type": "control", "nodeId": "osc", "nodePortId": "reset" }
      ],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].inputs[1].id",
        "Sound boundary IDs should be unique per direction");

    requireLoadErrorAt(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [
        { "id": "frequency", "type": "unknown", "nodeId": "osc", "nodePortId": "frequency" }
      ],
      "outputs": [],
      "graph": { "nodes": [], "connections": [] }
    }
  ])json"),
        "$.soundDefinitions[0].inputs[0].type",
        "Sound boundary types should be known");

    requireLoadErrorAt(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [],
      "outputs": [],
      "graph": { "nodes": [] }
    }
  ])json"),
        "$.soundDefinitions[0].graph.connections",
        "Sound definition graphs should require connections");
}

void testVersionOneRampMigratesToLinearRamp()
{
    const auto loaded = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": { "title": "Legacy Ramp" },
  "graph": {
    "nodes": [
      { "id": "duration", "typeId": "talas.constant", "parameters": [{ "id": "value", "expression": "2" }] },
      { "id": "start", "typeId": "talas.constant", "parameters": [{ "id": "value", "expression": "-1" }] },
      { "id": "end", "typeId": "talas.constant", "parameters": [{ "id": "value", "expression": "1" }] },
      { "id": "ramp", "typeId": "talas.ramp", "parameters": [
        { "id": "duration", "expression": "1 s" },
        { "id": "start", "expression": "0" },
        { "id": "end", "expression": "4" }
      ] }
    ],
    "connections": [
      { "fromNode": "duration", "fromPort": "value", "toNode": "ramp", "toPort": "duration" },
      { "fromNode": "start", "fromPort": "value", "toNode": "ramp", "toPort": "start" },
      { "fromNode": "end", "fromPort": "value", "toNode": "ramp", "toPort": "end" }
    ]
  }
})json");

    require(loaded.ok(), "version-one legacy Ramp project should load");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion, "version-one project should migrate to the current version in memory");
    require(loaded.document.graph.nodes[3].typeId == "talas.linearRamp", "legacy Ramp should migrate to LinearRamp");
    require(loaded.document.graph.nodes[3].parameters.size() == 3, "legacy Ramp parameters should survive migration");
    require(loaded.document.graph.connections.size() == 3, "legacy Ramp connections should survive migration");
    require(loaded.document.graph.connections[1].toPort == "start", "legacy Ramp start connection should remain unchanged");
    const auto serialized = talas::project::serializeProjectDocument(loaded.document);
    requireContains(serialized, R"json("version": 156)json", "migrated project should save as the current version");
    requireContains(serialized, R"json("typeId": "talas.linearRamp")json", "migrated project should save LinearRamp identity");
}

void testVersionTwoRampIdentityRoundTripsWithoutMigration()
{
    const auto loaded = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 2,
  "metadata": { "title": "Triggered Ramp" },
  "graph": {
    "nodes": [{ "id": "ramp", "typeId": "talas.ramp", "parameters": [
      { "id": "trigger", "expression": "!Gate" },
      { "id": "duration", "expression": "1 s" },
      { "id": "variant", "choice": "fullRamp" }
    ] }],
    "connections": []
  }
})json");

    require(loaded.ok(), "version-two triggered Ramp project should load");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion, "version-two project should migrate to the current version in memory");
    require(loaded.document.graph.nodes[0].typeId == "talas.ramp", "version-two Ramp must not be heuristically migrated");
    requireContains(talas::project::serializeProjectDocument(loaded.document), R"json("typeId": "talas.ramp")json",
                    "version-two Ramp should preserve its identity on save");
}

void testVersionThreeTimelineMigratesToVersionFourWithoutAssets()
{
    const auto loaded = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 3,
  "metadata": { "title": "Timeline v3" },
  "timeline": {
    "eventValues": [
      { "timeSeconds": 0.5, "eventName": "Gain", "domain": "scalar", "value": 0.75 }
    ]
  },
  "graph": { "nodes": [], "connections": [] }
})json");

    require(loaded.ok(), "version-three Timeline project should load");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
            "version-three project should migrate to version eleven in memory");
    require(loaded.document.assets.empty(), "version-three migration should create an empty asset registry");
    require(loaded.document.timeline.eventValues.size() == 1
                && loaded.document.timeline.eventValues.front().eventName == "Gain",
            "version-three migration should preserve Timeline events");
}

void testLegacyVersionsIgnoreFutureAssetFields()
{
    for (const auto version : {1, 2, 3})
    {
        const auto loaded = talas::project::parseProjectDocument(
            std::string(R"json({
  "format": "talas.project",
  "version": )json")
            + std::to_string(version)
            + R"json(,
  "metadata": { "title": "Legacy with future fields" },
  "assets": [
    { "id": "", "kind": "unknown", "path": "" }
  ],
  "graph": { "nodes": [], "connections": [] }
})json");

        require(loaded.ok(), "legacy project should ignore future asset fields");
        require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
                "legacy project should migrate to version eleven");
        require(loaded.document.assets.empty(),
                "legacy project migration should always create an empty asset registry");
    }
}

void testSerializedNodesUseTypeIdOnly()
{
    const auto json = talas::project::serializeProjectDocument(makePatchDocument());

    requireContains(json, R"json("typeId": "talas.oscillator")json", "serialized nodes should use typeId");
    requireNotContains(json, "Oscillator", "serialized nodes should not use display names");
    requireNotContains(json, "displayName", "serialized nodes should not include displayName");
}

void testStringEscapingRoundTrips()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Title with \"quotes\", backslash \\, and newline\nnext line";
    document.graph.nodes.push_back({
        "note1",
        "talas.annotation",
        {textParameter("body", "say \"hi\" \\ now\nthen")}
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json(\"quotes\")json", "quotes should be escaped");
    requireContains(json, R"json(\\)json", "backslashes should be escaped");
    requireContains(json, R"json(\n)json", "newlines should be escaped");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "escaped project should parse");
    require(loaded.document.metadata.title == document.metadata.title, "escaped title should round-trip");
    require(loaded.document.graph.nodes[0].parameters[0].text == document.graph.nodes[0].parameters[0].text,
            "escaped text parameter should round-trip");
}

void testChoiceParameterRoundTripsThroughJson()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "pan1",
        "talas.pan",
        {choiceParameter("type", "linear")}
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("choice": "linear")json", "choice parameter should serialize as choice");
    requireNotContains(json, R"json("expression": "linear")json", "choice parameter should not serialize as expression");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "choice parameter JSON should parse");
    require(loaded.document.graph.nodes[0].parameters[0].parameterId == "type", "choice parameter id should round-trip");
    require(loaded.document.graph.nodes[0].parameters[0].choice.has_value(), "choice parameter should round-trip choice value");
    require(*loaded.document.graph.nodes[0].parameters[0].choice == "linear", "choice parameter value should round-trip");
}

void testBooleanParameterRoundTripsThroughJson()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sample1",
        "talas.sample",
        {booleanParameter("setLoop", true)}
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("boolean": true)json", "boolean parameter should serialize as a JSON boolean");
    requireNotContains(json, R"json("expression": "true")json", "boolean parameter should not serialize as expression");
    requireNotContains(json, R"json("choice": "true")json", "boolean parameter should not serialize as choice");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "boolean parameter JSON should parse");
    require(loaded.document.graph.nodes[0].parameters[0].parameterId == "setLoop", "boolean parameter id should round-trip");
    require(loaded.document.graph.nodes[0].parameters[0].boolean.has_value(), "boolean parameter should round-trip boolean value");
    require(*loaded.document.graph.nodes[0].parameters[0].boolean, "boolean parameter value should round-trip");
}

void testResourceAndTextParametersRoundTripThroughJson()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sample1",
        "talas.sample",
        {
            resourceParameter("sample", "Samples/Kick 01.aif"),
            textParameter("comment", "Layer A\nLayer B"),
        }
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("resource": "Samples/Kick 01.aif")json", "resource parameter should serialize as resource");
    requireContains(json, R"json("text": "Layer A\nLayer B")json", "text parameter should serialize escaped text");
    requireNotContains(json, R"json("expression": "Samples/Kick 01.aif")json", "resource parameter should not serialize as expression");
    requireNotContains(json, R"json("choice": "Samples/Kick 01.aif")json", "resource parameter should not serialize as choice");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "resource/text parameter JSON should parse");
    require(loaded.document.graph.nodes[0].parameters[0].resource.has_value(), "resource parameter should round-trip resource value");
    require(*loaded.document.graph.nodes[0].parameters[0].resource == "Samples/Kick 01.aif", "resource parameter value should round-trip");
    require(loaded.document.graph.nodes[0].parameters[1].text.has_value(), "text parameter should round-trip text value");
    require(*loaded.document.graph.nodes[0].parameters[1].text == "Layer A\nLayer B", "text parameter value should round-trip");
}

void testMidiMapperModesRoundTripAndBlankMigratesCanonically()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Bounded MIDI Mapper modes";
    document.graph.nodes.push_back({
        "gateMapper",
        "talas.midiMapper",
        {
            textParameter("map", "gate"),
            expressionParameter("channel", "0"),
            expressionParameter("polyphony", "4"),
        },
    });
    document.graph.nodes.push_back({
        "velocityMapper",
        "talas.midiMapper",
        {
            textParameter("map", "velocity"),
            expressionParameter("channel", "16"),
            expressionParameter("polyphony", "128"),
        },
    });

    const auto canonical =
        talas::project::serializeProjectDocument(document);
    const auto reopened =
        talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && reopened.document.graph.nodes.size() == 2
            && reopened.document.graph.nodes[0].parameters[0].text
                == std::optional<std::string>{"gate"}
            && reopened.document.graph.nodes[1].parameters[0].text
                == std::optional<std::string>{"velocity"},
        "current-format MIDI Mapper modes should round-trip exact text authority");
    require(
        talas::project::serializeProjectDocument(reopened.document)
            == canonical,
        "current-format MIDI Mapper JSON should be byte-canonical after reopen");

    const auto legacy = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": { "title": "Legacy blank MIDI Mapper" },
  "graph": {
    "nodes": [
      {
        "id": "mapper",
        "typeId": "talas.midiMapper",
        "parameters": [
          { "id": "map", "text": "" },
          { "id": "left", "expression": "1" },
          { "id": "right", "expression": "1" }
        ]
      }
    ],
    "connections": []
  }
})json");
    require(
        legacy.ok()
            && legacy.document.formatVersion == talas::project::currentProjectFormatVersion
            && legacy.document.graph.nodes.front().typeId
                == "talas.midiMapper"
            && legacy.document.graph.nodes.front().parameters.front().text
                == std::optional<std::string>{""},
        "earliest supported blank MIDI Mapper source should migrate without inventing a mapped mode");
    const auto migrated =
        talas::project::serializeProjectDocument(legacy.document);
    const auto migratedReopened =
        talas::project::parseProjectDocument(migrated);
    require(
        migratedReopened.ok()
            && migratedReopened.document.graph.nodes.front()
                    .parameters.front().text
                == std::optional<std::string>{""}
            && talas::project::serializeProjectDocument(
                   migratedReopened.document)
                == migrated,
        "blank MIDI Mapper migration should remain byte-canonical and blank after reopen");
}

void testSerializerPreservesInvalidExpressionAndChoiceParameter()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "pan1",
        "talas.pan",
        {expressionAndChoiceParameter("type", "0", "linear")}
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("expression": "0")json",
                    "invalid expression and choice parameter should serialize the expression");
    requireContains(json, R"json("choice": "linear")json",
                    "invalid expression and choice parameter should serialize the choice");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(!loaded.ok(), "serialized invalid expression and choice parameter should still fail parse");
}

void testSerializerPreservesInvalidExpressionBooleanAndChoiceBooleanParameters()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sample1",
        "talas.sample",
        {
            expressionAndBooleanParameter("setLoop", "0", true),
            choiceAndBooleanParameter("loopMode", "on", false),
        }
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("expression": "0")json",
                    "invalid expression and boolean parameter should serialize the expression");
    requireContains(json, R"json("choice": "on")json",
                    "invalid choice and boolean parameter should serialize the choice");
    requireContains(json, R"json("boolean": true)json",
                    "invalid expression and boolean parameter should serialize the boolean");
    requireContains(json, R"json("boolean": false)json",
                    "invalid choice and boolean parameter should serialize the boolean");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(!loaded.ok(), "serialized invalid mixed boolean parameters should still fail parse");
}

void testSerializerPreservesInvalidResourceAndTextMixedParameters()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sample1",
        "talas.sample",
        {
            expressionAndResourceParameter("sample", "0", "Samples/Kick.aif"),
            booleanAndTextParameter("comment", true, "not numeric"),
        }
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("expression": "0")json",
                    "invalid expression and resource parameter should serialize the expression");
    requireContains(json, R"json("resource": "Samples/Kick.aif")json",
                    "invalid expression and resource parameter should serialize the resource");
    requireContains(json, R"json("boolean": true)json",
                    "invalid boolean and text parameter should serialize the boolean");
    requireContains(json, R"json("text": "not numeric")json",
                    "invalid boolean and text parameter should serialize the text");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(!loaded.ok(), "serialized invalid mixed resource/text parameters should still fail parse");
}

void testUnknownObjectFieldsAreIgnored()
{
    const std::string json = R"json({
  "format": "talas.project",
  "ignoredTopLevel": true,
  "version": 1,
  "metadata": {
    "title": "Known",
    "ignoredMetadata": "drop"
  },
  "graph": {
    "ignoredGraph": null,
    "nodes": [
      {
        "id": "osc1",
        "typeId": "talas.oscillator",
        "ignoredNode": 12,
        "parameters": [
          {
            "id": "frequency",
            "expression": "440",
            "ignoredParameter": false
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "osc1",
        "fromPort": "audioOut",
        "toNode": "out1",
        "toPort": "audioIn",
        "ignoredConnection": {}
      }
    ]
  }
})json";

    const auto loaded = talas::project::parseProjectDocument(json);

    require(loaded.ok(), "unknown JSON object fields should be ignored");
    require(loaded.document.metadata.title == "Known", "known metadata should still parse");
    require(loaded.document.graph.nodes.size() == 1, "known nodes should still parse");
    require(loaded.document.graph.nodes[0].parameters[0].expression.has_value(), "known parameter expression should parse as present");
    require(*loaded.document.graph.nodes[0].parameters[0].expression == "440", "known parameters should still parse");
    require(loaded.document.graph.connections[0].toNode == "out1", "known connections should still parse");
}

void testNodeCanvasPositionsRoundTripAndValidate()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"osc1", "talas.oscillator", {}});
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "osc1")->position = {320, -48};

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("nodeId": "osc1",
            "x": 320,
            "y": -48)json", "canvas position should serialize in portable presentation");
    requireNotContains(json, R"json("position")json", "format-eleven node topology should omit inline position");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "canvas position should parse");
    require(nodePosition(loaded.document, "osc1").x == 320, "canvas x should round-trip");
    require(nodePosition(loaded.document, "osc1").y == -48, "canvas y should round-trip");

    const auto legacy = talas::project::parseProjectDocument(R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": { "title": "" },
  "graph": {
    "nodes": [{ "id": "osc1", "typeId": "talas.oscillator", "parameters": [] }],
    "connections": []
  }
})json");
    require(legacy.ok(), "legacy project without positions should parse");
    require(nodePosition(legacy.document, "osc1") == talas::project::CanvasPosition{},
            "legacy positions should default to origin");

    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"y":0},"parameters":[]}],"connections":[]}})json",
                     "position should require x");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":0,"y":"bad"},"parameters":[]}],"connections":[]}})json",
                     "position y should be numeric");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":0.5,"y":0},"parameters":[]}],"connections":[]}})json",
                     "position coordinates should be integral");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":1.00000000000000001,"y":0},"parameters":[]}],"connections":[]}})json",
                     "position coordinates should not become integral through floating-point rounding");

    const auto exactIntegralForms = talas::project::parseProjectDocument(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":1.000,"y":1e2},"parameters":[]}],"connections":[]}})json");
    require(exactIntegralForms.ok(), "integral decimal and exponent forms should parse");
    require(nodePosition(exactIntegralForms.document, "osc1").x == 1, "integral decimal position should parse exactly");
    require(nodePosition(exactIntegralForms.document, "osc1").y == 100, "integral exponent position should parse exactly");

    const auto integerBounds = talas::project::parseProjectDocument(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":2147483647,"y":-2147483648},"parameters":[]}],"connections":[]}})json");
    require(integerBounds.ok(), "signed 32-bit coordinate bounds should parse");
    require(nodePosition(integerBounds.document, "osc1").x == 2147483647, "maximum signed 32-bit x should parse");
    require(nodePosition(integerBounds.document, "osc1").y == -2147483647 - 1, "minimum signed 32-bit y should parse");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","position":{"x":2147483648,"y":0},"parameters":[]}],"connections":[]}})json",
                     "position coordinates above signed 32-bit range should fail");
}

void testVersionTenPositionsMigrateToFormatElevenStateLayers()
{
    constexpr std::string_view source = R"json({
  "format": "talas.project",
  "version": 10,
  "metadata": { "title": "State migration" },
  "virtualControls": [
    {
      "eventName": "Gain",
      "domain": "scalar",
      "kind": "fader",
      "value": 0.5,
      "minimum": 0,
      "maximum": 1,
      "position": { "x": 640, "y": 96 }
    }
  ],
  "assets": [],
  "timeline": {
    "loop": { "enabled": false, "startSeconds": 0, "endSeconds": 4 },
    "eventValues": []
  },
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
    "outputChannels": [],
    "midiInputDevices": []
  },
  "soundDefinitions": [],
  "graph": {
    "nodes": [
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "position": { "x": 320, "y": -48 },
        "parameters": []
      }
    ],
    "connections": []
  }
})json";
    const auto loaded = talas::project::parseProjectDocument(source);

    require(loaded.ok(), "format-ten state migration fixture should load");
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion,
        "format-ten state migration should produce the current format-twelve model");
    require(
        loaded.document.presentation.structureLayouts.size() == 1
            && !loaded.document.presentation.structureLayouts.front()
                    .soundDefinitionId.has_value()
            && loaded.document.presentation.structureLayouts.front().nodes
                    == std::vector<talas::project::ProjectNodePresentation>{
                        {"osc", {320, -48}}}
            && loaded.document.presentation.performanceControls.size() == 1
            && loaded.document.presentation.performanceControls.front().target.kind
                == talas::project::SnapshotTargetKind::EventValue
            && loaded.document.presentation.performanceControls.front().target.eventName
                == "Gain"
            && loaded.document.presentation.performanceControls.front().target.domain
                == talas::expr::ValueDomain::Scalar
            && loaded.document.presentation.performanceControls.front().position
                == talas::project::CanvasPosition{640, 96}
            && loaded.document.presentation.performanceControls.front().visible,
        "format-ten positions should migrate into exact portable presentation identities");

    const auto canonical = talas::project::serializeProjectDocument(loaded.document);
    requireContains(canonical, R"json("version": 156)json", "canonical project should use the current format");
    requireContains(canonical, R"json("presentation": {)json", "portable presentation should have one top-level authority");
    requireContains(canonical, R"json("structureLayouts": [)json", "Structure layouts should serialize separately from topology");
    requireContains(canonical, R"json("nodeId": "osc")json", "migrated node position should retain stable node identity");
    requireContains(canonical, R"json("x": 320)json", "migrated Structure x should be exact");
    requireContains(canonical, R"json("y": -48)json", "migrated Structure y should be exact");
    requireContains(canonical, R"json("performanceControls": [)json", "Performance layout should serialize separately from controls");
    requireContains(canonical, R"json("eventName": "Gain")json", "migrated control position should retain stable Event Value identity");
    requireContains(canonical, R"json("x": 640)json", "migrated Performance x should be exact");
    requireContains(canonical, R"json("y": 96)json", "migrated Performance y should be exact");
    requireContains(canonical, R"json("snapshotLibrary": {)json", "format twelve should retain the versioned snapshot boundary");
    requireContains(canonical, R"json("banks": [])json", "migrated pre-snapshot projects should acquire an empty version-one library");
    requireNotContains(canonical, R"json("position")json", "format-twelve topology and controls should not retain inline positions");

    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok(), "canonical format-twelve project should reopen");
    require(
        reopened.document.presentation == loaded.document.presentation,
        "format-twelve presentation should round-trip exactly");
    require(
        talas::project::serializeProjectDocument(reopened.document) == canonical,
        "canonical format-twelve serialization should be byte-stable");

    auto nonEmptySnapshot = canonical;
    const auto banks = nonEmptySnapshot.find(R"json("banks": [])json");
    require(banks != std::string::npos, "canonical format twelve should expose empty banks");
    nonEmptySnapshot.replace(
        banks,
        std::string_view{R"json("banks": [])json"}.size(),
        R"json("banks": [{}])json");
    requireLoadErrorAt(
        nonEmptySnapshot,
        "$.snapshotLibrary.banks",
        "format-twelve snapshot banks should reject malformed populated entries");

    auto inlinePosition = canonical;
    const auto parameters = inlinePosition.find(R"json("typeId": "talas.oscillator")json");
    require(parameters != std::string::npos, "canonical fixture should expose its node");
    inlinePosition.insert(
        parameters,
        R"json("position": {"x": 1, "y": 2},
        )json");
    requireLoadErrorAt(
        inlinePosition,
        "$.graph.nodes[0].position",
        "format twelve should reject inline topology positions");

    auto legacyWithFutureState = std::string{source};
    const auto controls = legacyWithFutureState.find(R"json("virtualControls")json");
    require(controls != std::string::npos, "legacy fixture should expose virtual controls");
    legacyWithFutureState.insert(
        controls,
        R"json("presentation": {"version": 1, "structureLayouts": [], "performanceControls": []},
  )json");
    requireLoadErrorAt(
        legacyWithFutureState,
        "$.presentation",
        "format ten should reject format-eleven presentation fields");
}

void testLegacyNestedPositionsMigrateToScopedPresentation()
{
    const auto loaded = talas::project::parseProjectDocument(
        projectWithSoundDefinitions(R"json([
    {
      "id": "voice",
      "name": "Voice",
      "inputs": [],
      "outputs": [],
      "graph": {
        "nodes": [
          {
            "id": "osc",
            "typeId": "talas.oscillator",
            "position": { "x": 144, "y": -72 },
            "parameters": []
          }
        ],
        "connections": []
      }
    }
  ])json"));
    require(loaded.ok(), "legacy nested-position fixture should migrate");
    require(
        loaded.document.presentation.structureLayouts.size() == 2
            && !loaded.document.presentation.structureLayouts[0]
                    .soundDefinitionId.has_value()
            && loaded.document.presentation.structureLayouts[0].nodes.empty()
            && loaded.document.presentation.structureLayouts[1]
                    .soundDefinitionId
                == "voice"
            && loaded.document.presentation.structureLayouts[1].nodes
                == std::vector<talas::project::ProjectNodePresentation>{
                    {"osc", {144, -72}}},
        "nested positions should migrate into their definition-local scope");

    const auto canonical = talas::project::serializeProjectDocument(loaded.document);
    requireContains(
        canonical,
        R"json("definitionId": "voice")json",
        "nested presentation should serialize its stable definition scope");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.presentation == loaded.document.presentation,
        "nested format-eleven presentation should round-trip exactly");
}

void testFormatElevenWriterRequiresCompletePerformanceIdentity()
{
    talas::project::ProjectDocument document;
    talas::project::VirtualControl control;
    control.eventName = "Gain";
    control.domain = talas::expr::ValueDomain::Scalar;
    document.virtualControls.push_back(control);
    talas::project::reconcileProjectPresentation(document);
    document.presentation.performanceControls.front().position = {96, -24};
    document.presentation.performanceControls.front().target.domain =
        talas::expr::ValueDomain::Hertz;
    document.presentation.performanceControls.front().position = {999, 777};

    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok(),
        "writer should not publish a presentation entry with a mismatched Event Value domain");
    require(
        reopened.document.presentation.performanceControls.size() == 1
            && reopened.document.presentation.performanceControls.front()
                .target.eventName == "Gain"
            && reopened.document.presentation.performanceControls.front()
                .target.domain == talas::expr::ValueDomain::Scalar
            && reopened.document.presentation.performanceControls.front()
                .position == talas::project::CanvasPosition{},
        "writer fallback should preserve the complete semantic control identity");
}

void testFormatElevenWriterCanonicalizesIdentityKeyedPresentation()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"root-a", "talas.oscillator", {}});
    document.graph.nodes.push_back({"root-b", "talas.oscillator", {}});
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.graph.nodes.push_back(
        {"nested", "talas.oscillator", {}});
    document.soundDefinitions.push_back(std::move(definition));

    talas::project::VirtualControl first;
    first.eventName = "Alpha";
    document.virtualControls.push_back(first);
    talas::project::VirtualControl second;
    second.eventName = "Beta";
    document.virtualControls.push_back(second);
    talas::project::reconcileProjectPresentation(document);

    document.presentation.structureLayouts[0].nodes[0].position = {101, 102};
    document.presentation.structureLayouts[0].nodes[1].position = {201, 202};
    document.presentation.structureLayouts[1].nodes[0].position = {301, 302};
    document.presentation.performanceControls[0].position = {401, 402};
    document.presentation.performanceControls[1].position = {501, 502};
    document.presentation.performanceControls[1].visible = false;
    std::ranges::reverse(document.presentation.structureLayouts[0].nodes);
    std::ranges::reverse(document.presentation.structureLayouts);
    std::ranges::reverse(document.presentation.performanceControls);

    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok(),
        "writer should canonicalize reordered identity-keyed presentation arrays");
    const auto& presentation = reopened.document.presentation;
    require(
        presentation.structureLayouts
            == std::vector<talas::project::ProjectStructureLayout>{
                {std::nullopt,
                 {{"root-a", {101, 102}}, {"root-b", {201, 202}}}},
                {"voice", {{"nested", {301, 302}}}}},
        "writer should retain node presentation by scoped identity while canonicalizing order");
    require(
        presentation.performanceControls.size() == 2
            && presentation.performanceControls[0].target.eventName == "Alpha"
            && presentation.performanceControls[0].position
                == talas::project::CanvasPosition{401, 402}
            && presentation.performanceControls[0].visible
            && presentation.performanceControls[1].target.eventName == "Beta"
            && presentation.performanceControls[1].position
                == talas::project::CanvasPosition{501, 502}
            && !presentation.performanceControls[1].visible,
        "writer should retain control layout and visibility by complete identity while canonicalizing order");
    require(
        talas::project::serializeProjectDocument(reopened.document) == canonical,
        "canonicalized identity-keyed presentation should be byte-stable");
}

void testVirtualControlMetadataRoundTrips()
{
    talas::project::ProjectDocument document;
    talas::project::VirtualControl control;
    control.eventName = "Gain";
    control.domain = talas::expr::ValueDomain::Scalar;
    control.kind = talas::project::VirtualControlKind::Fader;
    control.value = 0.5;
    control.minimum = 0.0;
    control.maximum = 1.0;
    control.midiMapping = talas::project::MidiControlMapping{1, 12};
    document.virtualControls.push_back(std::move(control));
    talas::project::reconcileProjectPresentation(document);
    document.presentation.performanceControls.front().position = {320, 480};

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("virtualControls": [)json", "virtual controls should serialize");
    requireContains(json, R"json("eventName": "Gain")json", "virtual control event name should serialize");
    requireContains(json, R"json("controller": 12)json", "virtual control MIDI mapping should serialize");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "virtual controls should parse");
    require(loaded.document.virtualControls.size() == 1, "virtual control count should round-trip");
    const auto& loadedControl = loaded.document.virtualControls.front();
    require(loadedControl.eventName == "Gain", "virtual control event name should round-trip");
    require(loadedControl.kind == talas::project::VirtualControlKind::Fader, "virtual control kind should round-trip");
    require(std::abs(loadedControl.value - 0.5) <= 1.0e-12, "virtual control value should round-trip");
    require(loadedControl.midiMapping.has_value() && loadedControl.midiMapping->controller == 12,
            "virtual control MIDI mapping should round-trip");
    require(controlPosition(
                loaded.document,
                "Gain",
                talas::expr::ValueDomain::Scalar)
                == talas::project::CanvasPosition{320, 480},
            "virtual control position should round-trip");
}

void testFormatFourteenMidiSourceAuthorityRoundTripsAndMigrates()
{
    talas::project::ProjectDocument document;
    talas::project::VirtualControl control;
    control.eventName = "Cutoff";
    control.midiMapping = talas::project::MidiControlMapping{
        2,
        74,
        talas::project::MidiControlSourceMode::ExactInput,
        "physical-midi-id"};
    document.virtualControls.push_back(control);
    talas::project::reconcileProjectPresentation(document);

    const auto canonical = talas::project::serializeProjectDocument(document);
    requireContains(
        canonical,
        R"json("sourceMode": "exactInput")json",
        "format fourteen should persist exact MIDI source mode");
    requireContains(
        canonical,
        R"json("sourceIdentifier": "physical-midi-id")json",
        "format fourteen should persist the exact MIDI identifier");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.virtualControls.front().midiMapping
                == control.midiMapping,
        "exact MIDI source authority should round-trip canonically");

    auto legacy = canonical;
    const auto version = legacy.find(R"json("version": 156)json");
    require(version != std::string::npos, "current fixture should expose version");
    legacy.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  13)json");
    eraseFormatSeventeenTransportSync(legacy);
    eraseFormatEighteenOscIngress(legacy);
    eraseFormatSixteenMusicalTime(legacy);
    eraseExpressionLanguage(legacy);
    downgradePresentationToV1(legacy, document);
    const auto modeLine = legacy.find(
        R"json(        "sourceMode": "exactInput",
)json");
    const auto identifierLine = legacy.find(
        R"json(        "sourceIdentifier": "physical-midi-id",
)json");
    require(
        modeLine != std::string::npos && identifierLine != std::string::npos,
        "legacy fixture should expose format-fourteen MIDI fields");
    legacy.erase(identifierLine,
        std::string_view{R"json(        "sourceIdentifier": "physical-midi-id",
)json"}.size());
    legacy.erase(modeLine,
        std::string_view{R"json(        "sourceMode": "exactInput",
)json"}.size());
    const auto migrated = talas::project::parseProjectDocument(legacy);
    require(
        migrated.ok()
            && migrated.document.virtualControls.front().midiMapping->sourceMode
                == talas::project::MidiControlSourceMode::LegacyAnyInput
            && migrated.document.virtualControls.front()
                .midiMapping->sourceIdentifier.empty(),
        "format thirteen channel/CC mappings should migrate to explicit legacy-any authority");
    requireContains(
        talas::project::serializeProjectDocument(migrated.document),
        R"json("sourceMode": "legacyAnyInput")json",
        "canonical migration should make compatibility authority visible");

    auto malformed = canonical;
    const auto exact = malformed.find(R"json("sourceIdentifier": "physical-midi-id")json");
    malformed.replace(
        exact,
        std::string_view{R"json("sourceIdentifier": "physical-midi-id")json"}.size(),
        R"json("sourceIdentifier": "")json");
    requireLoadErrorAt(
        malformed,
        "$.virtualControls[0].midi",
        "format fourteen should reject empty exact MIDI source authority");

    auto oversized = canonical;
    const auto oversizedIdentifier = oversized.find(
        R"json("sourceIdentifier": "physical-midi-id")json");
    require(
        oversizedIdentifier != std::string::npos,
        "oversized MIDI fixture should expose its identifier");
    oversized.replace(
        oversizedIdentifier,
        std::string_view{
            R"json("sourceIdentifier": "physical-midi-id")json"}.size(),
        "\"sourceIdentifier\": \""
            + std::string(
                talas::project::maximumMidiSourceIdentifierBytes + 1, 'x')
            + "\"");
    requireLoadError(
        oversized,
        "format fourteen should reject oversized exact MIDI authority before model copy");

    auto escapedOversized = canonical;
    const auto escapedIdentifier = escapedOversized.find(
        R"json("sourceIdentifier": "physical-midi-id")json");
    escapedOversized.replace(
        escapedIdentifier,
        std::string_view{
            R"json("sourceIdentifier": "physical-midi-id")json"}.size(),
        "\"sourceIdentifier\": \""
            + std::string(
                talas::project::maximumMidiSourceIdentifierBytes + 1,
                'x')
            + "\"");
    for (std::size_t offset = escapedOversized.find(
             R"json("sourceIdentifier": ")json")
             + std::string_view{R"json("sourceIdentifier": ")json"}.size();
         offset < escapedOversized.size()
             && escapedOversized[offset] != '"';)
    {
        escapedOversized.replace(offset, 1, "\\u0078");
        offset += 6;
    }
    requireLoadError(
        escapedOversized,
        "format fourteen should bound decoded escaped MIDI authority before model copy");

    auto unknownMode = canonical;
    const auto exactMode = unknownMode.find(
        R"json("sourceMode": "exactInput")json");
    unknownMode.replace(
        exactMode,
        std::string_view{R"json("sourceMode": "exactInput")json"}.size(),
        R"json("sourceMode": "futureMode")json");
    requireLoadErrorAt(
        unknownMode,
        "$.virtualControls[0].midi",
        "format fourteen should reject unknown MIDI source modes");

    auto nonemptyLegacy = canonical;
    const auto legacyMode = nonemptyLegacy.find(
        R"json("sourceMode": "exactInput")json");
    nonemptyLegacy.replace(
        legacyMode,
        std::string_view{R"json("sourceMode": "exactInput")json"}.size(),
        R"json("sourceMode": "legacyAnyInput")json");
    requireLoadErrorAt(
        nonemptyLegacy,
        "$.virtualControls[0].midi",
        "format fourteen should reject nonempty legacy MIDI authority");

    for (const auto& [field, accepted, fractional] :
         std::array{
             std::tuple{"channel", "2", "2.00000000000000001"},
             std::tuple{"controller", "74", "74.000000000000001"},
         })
    {
        auto roundedFraction = canonical;
        const auto token = "\"" + std::string(field) + "\": "
            + accepted;
        const auto position = roundedFraction.find(token);
        require(
            position != std::string::npos,
            "fractional MIDI fixture should expose "
                + std::string(field));
        roundedFraction.replace(
            position, token.size(),
            "\"" + std::string(field) + "\": " + fractional);
        requireLoadErrorAt(
            roundedFraction,
            "$.virtualControls[0].midi",
            "format fourteen should reject binary64-rounded fractional MIDI "
                + std::string(field));
    }

    auto invalidUtf8 = canonical;
    const auto utf8Identifier = invalidUtf8.find("physical-midi-id");
    invalidUtf8.replace(
        utf8Identifier,
        std::string_view{"physical-midi-id"}.size(),
        std::string{"\xC3\x28", 2});
    requireLoadError(
        invalidUtf8,
        "format fourteen should reject invalid UTF-8 MIDI authority");
}

void testVirtualControlParsingRejectsInvalidModels()
{
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"virtualControls":[{"eventName":"Invalid-Name","domain":"scalar","kind":"fader","value":0.5,"minimum":0,"maximum":1,"position":{"x":0,"y":0}}],"graph":{"nodes":[],"connections":[]}})json",
                     "Event Value name should use identifier syntax");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"virtualControls":[{"eventName":"Gain","domain":"scalar","kind":"fader","value":1.5,"minimum":0,"maximum":1,"position":{"x":0,"y":0}}],"graph":{"nodes":[],"connections":[]}})json",
                     "Event Value value should be inside its range");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"virtualControls":[{"eventName":"Gain","domain":"scalar","kind":"fader","value":0.5,"minimum":0,"maximum":1,"midi":{"channel":1,"controller":74},"position":{"x":0,"y":0}},{"eventName":"Cutoff","domain":"scalar","kind":"fader","value":0.5,"minimum":0,"maximum":1,"midi":{"channel":1,"controller":74},"position":{"x":0,"y":0}}],"graph":{"nodes":[],"connections":[]}})json",
                     "Event Values should not share a MIDI mapping");
}

void testProjectAssetsRoundTripAndValidate()
{
    talas::project::ProjectDocument document;
    document.assets = {
        {"Samples/Hit", talas::project::ProjectAssetKind::Table, "assets/hit.wav"},
        {"Sequences/Part", talas::project::ProjectAssetKind::Binary, "../midi/part.mid"},
    };
    talas::project::ProjectAsset audio;
    audio.id = "Audio/Take";
    audio.kind = talas::project::ProjectAssetKind::Audio;
    audio.path = "audio/take.wav";
    audio.disposition = talas::project::ProjectAssetDisposition::Embedded;
    audio.contentHash.assign(64, 'a');
    audio.channelCount = 2;
    audio.sampleRate = 48000.0;
    audio.frameCount = 96000;
    audio.encoding = "pcm16-wav";
    audio.generation = 7;
    audio.provenance = "local capture";
    document.assets.push_back(audio);

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("assets": [)json", "project assets should serialize");
    requireContains(json, R"json("id": "Samples/Hit")json", "project asset id should serialize");
    requireContains(json, R"json("kind": "binary")json", "project asset kind should serialize");
    requireContains(json, R"json("path": "../midi/part.mid")json", "project asset path should serialize");
    requireContains(json, R"json("disposition": "embedded")json",
                    "asset disposition should serialize");
    requireContains(json, R"json("generation": 7)json",
                    "asset generation should serialize");
    requireContains(json, R"json("frameCount": 96000)json",
                    "asset media shape should serialize");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "serialized project assets should parse");
    require(loaded.document.formatVersion == talas::project::currentProjectFormatVersion, "asset project should load as the current version");
    require(loaded.document.assets.size() == 3, "project asset count should round-trip");
    require(loaded.document.assets[0].id == "Samples/Hit"
                && loaded.document.assets[0].kind == talas::project::ProjectAssetKind::Table
                && loaded.document.assets[0].path == "assets/hit.wav",
            "table project asset should round-trip");
    require(loaded.document.assets[1].kind == talas::project::ProjectAssetKind::Binary,
            "binary project asset should round-trip");
    require(loaded.document.assets[2] == audio,
            "every canonical audio manifest field should round-trip exactly");

    const std::string legacyTemplate = R"json({
  "format": "talas.project",
  "version": LEGACY_VERSION,
  "metadata": { "title": "Legacy asset" },
  "virtualControls": [],
  "assets": [
    { "id": "Samples/Legacy", "kind": "table", "path": "assets/legacy.wav" }
  ],
  "timeline": {
    "loop": { "enabled": false, "startSeconds": 0, "endSeconds": 4 },
    "eventValues": []
  },
  "deviceSettings": {
    "configured": false,
    "audioInputDevice": "",
    "audioOutputDevice": "",
    "sampleRate": 0,
    "bufferSize": 0,
    "inputChannels": [],
    "outputChannels": [],
    "midiInputDevices": []
  },
  "soundDefinitions": [],
  "graph": { "nodes": [], "connections": [] }
})json";
    for (int version = 4; version <= 9; ++version)
    {
        auto legacyJson = legacyTemplate;
        const auto marker = legacyJson.find("LEGACY_VERSION");
        legacyJson.replace(
            marker,
            std::string{"LEGACY_VERSION"}.size(),
            std::to_string(version));
        const auto legacy =
            talas::project::parseProjectDocument(legacyJson);
        require(
            legacy.ok()
                && legacy.document.formatVersion == talas::project::currentProjectFormatVersion
                && legacy.document.assets.size() == 1
                && legacy.document.assets.front().id
                    == "Samples/Legacy"
                && legacy.document.assets.front().path
                    == "assets/legacy.wav"
                && legacy.document.assets.front().generation == 1
                && legacy.document.assets.front().contentHash.empty()
                && legacy.document.assets.front().channelCount == 0
                && legacy.document.assets.front().sampleRate == 0.0
                && legacy.document.assets.front().frameCount == 0
                && legacy.document.assets.front().encoding.empty()
                && legacy.document.assets.front().provenance.empty()
                && legacy.document.assets.front().disposition
                    == talas::project::ProjectAssetDisposition::Referenced,
            "format 4-9 assets should migrate without changing references or inventing metadata");
    }

    requireLoadError(R"json({"format":"talas.project","version":4,"metadata":{"title":""},"assets":[{"id":"","kind":"table","path":"a.wav"}],"graph":{"nodes":[],"connections":[]}})json",
                     "project asset id should not be empty");
    requireLoadError(R"json({"format":"talas.project","version":4,"metadata":{"title":""},"assets":[{"id":"A","kind":"unknown","path":"a.wav"}],"graph":{"nodes":[],"connections":[]}})json",
                     "project asset kind should be known");
    requireLoadError(R"json({"format":"talas.project","version":4,"metadata":{"title":""},"assets":[{"id":"A","kind":"table","path":""}],"graph":{"nodes":[],"connections":[]}})json",
                     "project asset path should not be empty");
    requireLoadError(R"json({"format":"talas.project","version":4,"metadata":{"title":""},"assets":[{"id":"A","kind":"table","path":"a.wav"},{"id":"A","kind":"binary","path":"a.mid"}],"graph":{"nodes":[],"connections":[]}})json",
                     "project asset ids should be unique");

    auto invalidHash = audio;
    invalidHash.contentHash.assign(64, 'A');
    document.assets = {invalidHash};
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(document),
        "$.assets[0].contentHash",
        "format-ten hashes should be lowercase canonical SHA-256");
    auto unverifiedEmbedded = audio;
    unverifiedEmbedded.contentHash.clear();
    document.assets = {unverifiedEmbedded};
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(document),
        "$.assets[0].contentHash",
        "embedded format-ten assets should require a verified hash");
    auto invalidGeneration = audio;
    invalidGeneration.generation = 0;
    document.assets = {invalidGeneration};
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(document),
        "$.assets[0].generation",
        "format-ten generations should be positive");
    auto invalidBinaryShape = document.assets.front();
    invalidBinaryShape.generation = 1;
    invalidBinaryShape.kind = talas::project::ProjectAssetKind::Binary;
    invalidBinaryShape.channelCount = 2;
    invalidBinaryShape.sampleRate = 48000;
    invalidBinaryShape.frameCount = 10;
    invalidBinaryShape.encoding = "pcm16-wav";
    document.assets = {invalidBinaryShape};
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(document),
        "$.assets[0]",
        "non-audio assets should reject invented audio shape metadata");
    auto duplicateGeneration = audio;
    duplicateGeneration.generation = audio.generation + 1;
    document.assets = {audio, duplicateGeneration};
    requireLoadErrorAt(
        talas::project::serializeProjectDocument(document),
        "$.assets[1].id",
        "one stable asset id cannot publish two ambiguous generations in the active manifest");
}

void testTimelineEventValuesRoundTripAndValidate()
{
    talas::project::ProjectDocument document;
    document.timeline.eventValues = {
        {0.0, "Gate", talas::expr::ValueDomain::Scalar, 1.0,
         talas::core::AutomationInterpolation::Linear},
        {1.25, "Cutoff", talas::expr::ValueDomain::Hertz, 880.0},
    };

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("timeline": {)json", "Timeline should serialize");
    requireContains(json, R"json("timeSeconds": 1.25)json", "Timeline time should serialize in seconds");
    requireContains(json, R"json("domain": "hertz")json", "Timeline domain should serialize");
    requireContains(json, R"json("interpolation": "linear")json",
                    "Timeline linear interpolation should serialize");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "serialized Timeline should parse");
    require(loaded.document.timeline.eventValues.size() == 2, "Timeline event count should round-trip");
    require(loaded.document.timeline.eventValues[1].eventName == "Cutoff",
            "Timeline Event Value name should round-trip");
    require(loaded.document.timeline.eventValues[1].domain == talas::expr::ValueDomain::Hertz,
            "Timeline Event Value domain should round-trip");
    require(loaded.document.timeline.eventValues[0].interpolation
                == talas::core::AutomationInterpolation::Linear
                && loaded.document.timeline.eventValues[1].interpolation
                    == talas::core::AutomationInterpolation::Step,
            "Timeline interpolation should round-trip with a default step keyframe");
    require(std::abs(loaded.document.timeline.eventValues[1].timeSeconds - 1.25) <= 1.0e-12,
            "Timeline time should round-trip");

    const auto sampleExactTime = 100.49 / 48000.0;
    document.timeline.eventValues = {
        {sampleExactTime, "Gain", talas::expr::ValueDomain::Scalar, 0.12345678901234566},
    };
    const auto precise = talas::project::parseProjectDocument(
        talas::project::serializeProjectDocument(document));
    require(precise.ok(), "high-precision Timeline values should parse after serialization");
    require(precise.document.timeline.eventValues.front().timeSeconds == sampleExactTime,
            "Timeline serialization must preserve enough precision to retain sample rounding");
    require(precise.document.timeline.eventValues.front().value
                == document.timeline.eventValues.front().value,
            "Timeline Event Value serialization must round-trip the exact double");

    const auto versionSix = talas::project::parseProjectDocument(
        R"json({"format":"talas.project","version":6,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"loop":{"enabled":false,"startSeconds":0,"endSeconds":4},"eventValues":[{"timeSeconds":0,"eventName":"Gain","domain":"scalar","value":0.5}]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json");
    require(versionSix.ok(), "version-six Timeline should migrate");
    require(versionSix.document.formatVersion == talas::project::currentProjectFormatVersion
                && versionSix.document.timeline.eventValues.front().interpolation
                    == talas::core::AutomationInterpolation::Step,
            "version-six Timeline keyframes should migrate to step interpolation");

    requireLoadError(
        R"json({"format":"talas.project","version":7,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"loop":{"enabled":false,"startSeconds":0,"endSeconds":4},"eventValues":[{"timeSeconds":0,"eventName":"Gain","domain":"scalar","value":0.5}]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "version-seven Timeline interpolation should be required");
    requireLoadError(
        R"json({"format":"talas.project","version":7,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"loop":{"enabled":false,"startSeconds":0,"endSeconds":4},"eventValues":[{"timeSeconds":0,"eventName":"Gain","domain":"scalar","value":0.5,"interpolation":"spline"}]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "unknown Timeline interpolation should be rejected");

    requireLoadError(R"json({"format":"talas.project","version":3,"metadata":{"title":""},"timeline":{"eventValues":[{"timeSeconds":-1,"eventName":"Gate","domain":"scalar","value":1}]},"graph":{"nodes":[],"connections":[]}})json",
                     "Timeline time should be non-negative");
    requireLoadError(R"json({"format":"talas.project","version":3,"metadata":{"title":""},"timeline":{"eventValues":[{"timeSeconds":0,"eventName":"Invalid-Name","domain":"scalar","value":1}]},"graph":{"nodes":[],"connections":[]}})json",
                     "Timeline Event Value name should use identifier syntax");
    requireLoadError(R"json({"format":"talas.project","version":3,"metadata":{"title":""},"timeline":{"eventValues":[{"timeSeconds":0,"eventName":"Gate","domain":"unknown","value":1}]},"graph":{"nodes":[],"connections":[]}})json",
                     "Timeline domain should be known");
    requireLoadError(R"json({"format":"talas.project","version":3,"metadata":{"title":""},"timeline":{"eventValues":[{"timeSeconds":0,"eventName":"Gate","domain":"scalar","value":1e999}]},"graph":{"nodes":[],"connections":[]}})json",
                     "Timeline value should be finite");
    requireLoadError(R"json({"format":"talas.project","version":3,"metadata":{"title":""},"timeline":{"eventValues":[{"timeSeconds":1,"eventName":"Gate","domain":"scalar","value":1},{"timeSeconds":0.5,"eventName":"Gate","domain":"scalar","value":-1}]},"graph":{"nodes":[],"connections":[]}})json",
                     "Timeline events should be stored in chronological order");
}

void testFormatTwentyOneScoreRoundTripsAndRejectsMalformedAuthority()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voiceDefinition";
    definition.displayName = "Voice";
    definition.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    definition.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    document.soundDefinitions.push_back(std::move(definition));
    talas::project::ProjectNode sourceInstance;
    sourceInstance.instanceId = "voiceSource";
    sourceInstance.soundDefinitionId = "voiceDefinition";
    document.graph.nodes.push_back(std::move(sourceInstance));
    document.timeline.score.tracks = {
        {"voice",
         "Voice",
         talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::MusicalTicks,
         "master",
         2,
         talas::project::ScoreOverlapMode::Layer},
        {"master",
         "Master",
         talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds,
         {},
         2,
         talas::project::ScoreOverlapMode::Layer},
    };
    document.timeline.score.tracks.front().sharingMemoryWriters = true;
    const auto ticks = [](std::uint64_t value)
    { return talas::project::ScoreTime{talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value}; };
    document.timeline.score.clips.push_back({"intro",
                                             "voice",
                                             "voiceDefinition",
                                             ticks(12345),
                                             ticks(960),
                                             ticks(0),
                                             ticks(120),
                                             ticks(120),
                                             2});
    document.timeline.score.clips.front().sourceInstanceId = "voiceSource";
    document.timeline.score.automation.push_back(
        {"gainLane",
         talas::project::ScoreAutomationScope::Track,
         "voice",
         "Gain",
         talas::expr::ValueDomain::Scalar,
         {{ticks(12345), 0.25, talas::core::AutomationInterpolation::Linear}}});
    document.timeline.score.markers.push_back({"verse", "Verse", ticks(12345), ticks(13305)});

    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto removeSharingMemoryWriters = [](std::string& serialized) {
        for (const auto value : {"true", "false"})
        {
            const auto field = std::string{",\n          \"sharingMemoryWriters\": "}
                + value;
            for (auto position = serialized.find(field);
                 position != std::string::npos;
                 position = serialized.find(field))
                serialized.erase(position, field.size());
        }
    };
    const auto loaded = talas::project::parseProjectDocument(canonical);
    std::string scoreLoadDiagnostics;
    for (const auto& error : loaded.errors)
        scoreLoadDiagnostics += " [" + error.path + ": " + error.message + "]";
    require(
        loaded.ok(),
        "format-34 Score should parse after canonical serialization"
            + scoreLoadDiagnostics);
    require(loaded.document.timeline.score == document.timeline.score,
            "format-21 Score should round-trip every closed field exactly");
    require(talas::project::serializeProjectDocument(loaded.document) == canonical,
            "format-21 Score should remain byte-canonical after reopen");

    // Format 123: a lane Live or Record-enabled keeps its state; an
    // Automated lane writes none, and an older project cannot carry one.
    require(canonical.find("\"state\"") == std::string::npos,
            "an Automated lane should write no state");
    auto stated = document;
    stated.timeline.score.automation.front().state = talas::project::ScoreAutomationState::Record;
    const auto statedJson = talas::project::serializeProjectDocument(stated);
    requireContains(statedJson, R"json("state": "record")json", "a Record-enabled lane should write its state");
    const auto statedLoaded = talas::project::parseProjectDocument(statedJson);
    require(statedLoaded.ok() && statedLoaded.document.timeline.score == stated.timeline.score,
            "a lane's state should round-trip");
    auto olderState = statedJson;
    const auto versionField = olderState.find("\"version\": 156");
    require(versionField != std::string::npos, "the project should be format 123");
    olderState.replace(versionField, std::string_view{"\"version\": 156"}.size(), "\"version\": 122");
    requireLoadError(olderState, "a format-122 project should not carry a lane's state");
    auto automatedState = statedJson;
    const auto stateField = automatedState.find(R"json("state": "record")json");
    automatedState.replace(stateField, std::string_view{R"json("state": "record")json"}.size(), R"json("state": "automated")json");
    requireLoadError(automatedState, "an Automated lane should be written without a state");

    // Format 124: a clip's anchor, a marker's program change and the
    // Timeline's program change channel, each
    // written only when set; an older project cannot carry them.
    for (const auto* key : {"\"anchorOffset\"", "\"programNumber\"", "\"markerProgramChannel\""})
        require(canonical.find(key) == std::string::npos, std::string{"an unset "} + key + " should not be written");
    auto marked = document;
    marked.timeline.score.clips.front().anchorOffset = ticks(480);
    marked.timeline.score.markers.front().programNumber = 9;
    marked.timeline.markerProgramChannel = 16;
    const auto markedJson = talas::project::serializeProjectDocument(marked);
    requireContains(markedJson, R"json("programNumber": 9)json", "a marker's program change should be written");
    requireContains(markedJson, R"json("markerProgramChannel": 16)json", "the Timeline's program change channel should be written");
    const auto markedLoaded = talas::project::parseProjectDocument(markedJson);
    require(markedLoaded.ok() && markedLoaded.document.timeline.score == marked.timeline.score
                && markedLoaded.document.timeline.markerProgramChannel == 16,
            "an anchor, a program change and a program change channel should round-trip");
    for (const auto* key : {"\"anchorOffset\"", "\"programNumber\"", "\"markerProgramChannel\""})
    {
        auto older = marked;
        if (std::string_view{key} != "\"anchorOffset\"")
            older.timeline.score.clips.front().anchorOffset.reset();
        if (std::string_view{key} != "\"programNumber\"")
            older.timeline.score.markers.front().programNumber.reset();
        if (std::string_view{key} != "\"markerProgramChannel\"")
            older.timeline.markerProgramChannel = 0;
        auto olderJson = talas::project::serializeProjectDocument(older);
        const auto field = olderJson.find("\"version\": 156");
        require(field != std::string::npos, "the project should be format 124");
        olderJson.replace(field, std::string_view{"\"version\": 156"}.size(), "\"version\": 123");
        requireLoadError(olderJson, std::string{"a format-123 project should not carry "} + key);
    }
    auto outside = markedJson;
    const auto anchorField = outside.find("\"anchorOffset\"");
    const auto anchorTick = anchorField == std::string::npos ? std::string::npos : outside.find("480", anchorField);
    require(anchorTick != std::string::npos, "the clip's anchor should be written");
    outside.replace(anchorTick, 3, "1921");
    requireLoadError(outside, "an anchor past the clip's end should be refused");

    // Format 146: a clip's colour and the Timeline's own time code frames a
    // second, written only when set; an older
    // project cannot carry them, and other frame rates are refused.
    for (const auto* key : {"\"colour\"", "\"timeCodeFramesPerSecond\""})
        require(canonical.find(key) == std::string::npos, std::string{"an unset "} + key + " should not be written");
    auto coloured = document;
    coloured.timeline.score.clips.front().colour = 0x4A72D0U;
    coloured.timeline.timeCodeFramesPerSecond = 25;
    const auto colouredJson = talas::project::serializeProjectDocument(coloured);
    const auto colouredLoaded = talas::project::parseProjectDocument(colouredJson);
    require(colouredLoaded.ok() && colouredLoaded.document.timeline.score == coloured.timeline.score
            && colouredLoaded.document.timeline.timeCodeFramesPerSecond == 25,
        "a clip's colour and the Timeline's frames a second should round-trip");
    auto olderColoured = colouredJson;
    olderColoured.replace(olderColoured.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 145");
    requireLoadError(olderColoured, "a format-145 project should not carry a clip's colour or the Timeline's frames");
    auto oddFrames = colouredJson;
    oddFrames.replace(oddFrames.find("\"timeCodeFramesPerSecond\": 25"), std::string_view{"\"timeCodeFramesPerSecond\": 25"}.size(),
        "\"timeCodeFramesPerSecond\": 29");
    requireLoadError(oddFrames, "the Timeline's frames a second should be 24, 25 or 30");

    // Format 154: the word size of the Timeline's
    // caches, 24 bits unless the Audio menu sets 16, written when 16; an
    // older project caches in 16 bits as it did, and cannot carry it.
    require(document.timeline.cacheWordSize == 24 && canonical.find("\"cacheWordSize\"") == std::string::npos,
        "a Timeline's 24-bit caches, the default, should not be written");
    auto sixteen = document;
    sixteen.timeline.cacheWordSize = 16;
    const auto sixteenJson = talas::project::serializeProjectDocument(sixteen);
    const auto sixteenLoaded = talas::project::parseProjectDocument(sixteenJson);
    require(sixteenLoaded.ok() && sixteenLoaded.document.timeline.cacheWordSize == 16,
        "a Timeline's 16-bit caches should round-trip");
    auto olderSixteen = sixteenJson;
    olderSixteen.replace(olderSixteen.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 153");
    requireLoadError(olderSixteen, "a format-153 project should not carry the caches' word size");
    auto oddBits = sixteenJson;
    oddBits.replace(oddBits.find("\"cacheWordSize\": 16"), std::string_view{"\"cacheWordSize\": 16"}.size(),
        "\"cacheWordSize\": 20");
    requireLoadError(oddBits, "the caches' word size should be 16 or 24 bits");
    auto olderCanonical = canonical;
    olderCanonical.replace(olderCanonical.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 153");
    const auto olderLoaded = talas::project::parseProjectDocument(olderCanonical);
    require(olderLoaded.ok() && olderLoaded.document.timeline.cacheWordSize == 16,
        "a format-153 project should go on caching in 16 bits");

    // Format 156: recordings shared across the
    // Timeline's tracks, off unless set, written only when on; an older
    // project keeps each track's own and cannot carry it.
    require(!document.timeline.sharingMemoryWritersAcrossTracks
            && canonical.find("\"sharingMemoryWritersAcrossTracks\"") == std::string::npos,
        "recordings kept to their tracks, the default, should not be written");
    auto acrossTracks = document;
    acrossTracks.timeline.sharingMemoryWritersAcrossTracks = true;
    const auto acrossJson = talas::project::serializeProjectDocument(acrossTracks);
    const auto acrossLoaded = talas::project::parseProjectDocument(acrossJson);
    require(acrossLoaded.ok() && acrossLoaded.document.timeline.sharingMemoryWritersAcrossTracks
            && talas::project::serializeProjectDocument(acrossLoaded.document) == acrossJson,
        "recordings shared across the tracks should round-trip");
    auto olderAcross = acrossJson;
    olderAcross.replace(olderAcross.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155");
    requireLoadError(olderAcross, "a format-155 project should not share recordings across its tracks");
    auto falseAcross = acrossJson;
    falseAcross.replace(falseAcross.find("\"sharingMemoryWritersAcrossTracks\": true"),
        std::string_view{"\"sharingMemoryWritersAcrossTracks\": true"}.size(), "\"sharingMemoryWritersAcrossTracks\": false");
    requireLoadError(falseAcross, "the sharing across tracks is written only when on");
    auto olderShared = canonical;
    olderShared.replace(olderShared.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155");
    const auto olderSharedLoaded = talas::project::parseProjectDocument(olderShared);
    require(olderSharedLoaded.ok() && !olderSharedLoaded.document.timeline.sharingMemoryWritersAcrossTracks,
        "a format-155 project should keep each track's recordings to itself");

    auto cachedDocument = document;
    const auto makeCacheAsset = [](std::string id, std::string path) {
        talas::project::ProjectAsset asset;
        asset.id = std::move(id);
        asset.kind = talas::project::ProjectAssetKind::Audio;
        asset.path = std::move(path);
        asset.disposition = talas::project::ProjectAssetDisposition::Embedded;
        asset.contentHash.assign(64, 'a');
        asset.channelCount = 1;
        asset.sampleRate = 48000.0;
        asset.frameCount = 48000;
        asset.encoding = "pcm16-wav";
        asset.generation = 1;
        asset.provenance = "C8 clip cache test";
        return asset;
    };
    cachedDocument.assets = {
        makeCacheAsset("Caches/intro-left", "Caches/intro-left.wav"),
        makeCacheAsset("Caches/intro-right", "Caches/intro-right.wav"),
    };
    cachedDocument.timeline.score.clips.front().renderArtifact =
        talas::project::ScoreClipRenderArtifact{
            talas::project::ScoreClipRenderMode::Cached,
            std::string(talas::project::scoreClipRenderFingerprintBytes, 'b'),
            48000,
            48000,
            {"Caches/intro-left", "Caches/intro-right"},
        };
    const auto cachedCanonical =
        talas::project::serializeProjectDocument(cachedDocument);
    const auto cachedLoaded =
        talas::project::parseProjectDocument(cachedCanonical);
    require(
        cachedLoaded.ok()
            && cachedLoaded.document.timeline.score
                == cachedDocument.timeline.score
            && cachedLoaded.document.assets == cachedDocument.assets
            && cachedCanonical.find("\"mode\": \"cached\"")
                != std::string::npos
            && cachedCanonical.find(
                "\"assetId\": \"Caches/intro-right\"")
                != std::string::npos,
        "format 34 should round-trip exact bounded Score clip render authority");
    require(
        talas::project::serializeProjectDocument(cachedLoaded.document)
            == cachedCanonical,
        "format-34 Score clip render authority should remain byte-canonical after reopen");

    auto staleAssetReference = cachedCanonical;
    const auto artifactAsset = staleAssetReference.find(
        "\"assetId\": \"Caches/intro-right\"");
    require(
        artifactAsset != std::string::npos,
        "cache fixture should expose its second channel authority");
    staleAssetReference.replace(
        artifactAsset,
        std::string_view{
            "\"assetId\": \"Caches/intro-right\""}.size(),
        "\"assetId\": \"Caches/missing-right\"");
    requireLoadErrorAt(
        staleAssetReference,
        "$.timeline.score.clips[intro].renderArtifact",
        "format-34 cache authority must reject a missing channel asset");

    auto formatThirtyTwo = canonical;
    const auto currentVersion = formatThirtyTwo.find(
        R"json("version": 156)json");
    const auto nullArtifact = formatThirtyTwo.find(
        R"json(          "renderArtifact": null
)json");
    const auto exactSourceInstance = formatThirtyTwo.find(
        R"json(          "sourceInstanceId": "voiceSource",
)json");
    require(
        currentVersion != std::string::npos
            && nullArtifact != std::string::npos
            && exactSourceInstance != std::string::npos,
        "current live Score fixture should expose format, source, and null artifact authority");
    formatThirtyTwo.replace(
        currentVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  32)json");
    const auto currentLanguage = formatThirtyTwo.find(
        R"json("expressionLanguage": "variablesV2")json");
    require(
        currentLanguage != std::string::npos,
        "format-32 downgrade fixture should expose current VariablesV2 authority");
    formatThirtyTwo.replace(
        currentLanguage,
        std::string_view{
            R"json("expressionLanguage": "variablesV2")json"}.size(),
        R"json("expressionLanguage": "variablesV1")json");
    formatThirtyTwo.erase(
        nullArtifact,
        std::string_view{R"json(          "renderArtifact": null
)json"}.size());
    formatThirtyTwo.erase(
        exactSourceInstance,
        std::string_view{R"json(          "sourceInstanceId": "voiceSource",
)json"}.size());
    const auto repeated = formatThirtyTwo.find(
        R"json("repeatCount": 2,)json");
    require(
        repeated != std::string::npos,
        "format-32 downgrade fixture should expose the clip trailing comma");
    formatThirtyTwo.erase(
        repeated + std::string_view{R"json("repeatCount": 2)json"}.size(),
        1);
    eraseLiveSetLibrary(formatThirtyTwo);
    removeSharingMemoryWriters(formatThirtyTwo);
    const auto migratedThirtyTwo =
        talas::project::parseProjectDocument(formatThirtyTwo);
    require(
        migratedThirtyTwo.ok()
            && migratedThirtyTwo.document.formatVersion == talas::project::currentProjectFormatVersion
            && !migratedThirtyTwo.document.timeline.score.clips.front()
                    .renderArtifact.has_value()
            && !migratedThirtyTwo.document.timeline.score.clips.front()
                    .sourceInstanceId.has_value(),
        "format 32 should migrate without inventing Score clip source, cache, or freeze state"
            + (migratedThirtyTwo.errors.empty()
                    ? std::string{}
                    : ": " + migratedThirtyTwo.errors.front().path + " "
                        + migratedThirtyTwo.errors.front().message));

    auto missingSourceInstance = canonical;
    const auto exactSource = missingSourceInstance.find(
        R"json("sourceInstanceId": "voiceSource")json");
    require(
        exactSource != std::string::npos,
        "format-34 Score fixture should expose its exact source instance");
    missingSourceInstance.replace(
        exactSource,
        std::string_view{
            R"json("sourceInstanceId": "voiceSource")json"}.size(),
        R"json("sourceInstanceId": "missingSource")json");
    requireLoadErrorAt(
        missingSourceInstance,
        "$.timeline.score.clips[intro]",
        "format-34 Score must reject a missing exact source instance");

    auto formatThirtySix = canonical;
    const auto priorVersion = formatThirtySix.find(R"json("version": 156)json");
    require(priorVersion != std::string::npos,
        "shared-memory Score fixture should expose format 37");
    formatThirtySix.replace(priorVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  36)json");
    removeSharingMemoryWriters(formatThirtySix);
    const auto migratedThirtySix =
        talas::project::parseProjectDocument(formatThirtySix);
    require(migratedThirtySix.ok()
            && migratedThirtySix.document.formatVersion == talas::project::currentProjectFormatVersion
            && !migratedThirtySix.document.timeline.score.tracks.front()
                    .sharingMemoryWriters,
        "format 36 should migrate to private MemoryWriter tracks by default");

    auto missingSharingField = canonical;
    removeSharingMemoryWriters(missingSharingField);
    requireLoadErrorAt(missingSharingField,
        "$.timeline.score.tracks[0]",
        "format 37 requires an explicit track MemoryWriter sharing option");

    auto premature = canonical;
    const auto version = premature.find(R"json("version": 156)json");
    require(version != std::string::npos, "Score fixture should expose current version");
    premature.replace(
        version, std::string_view{R"json("version": 156)json"}.size(), R"json("version":  20)json");
    requireLoadErrorAt(
        premature, "$.timeline.score", "format-20 must reject a premature Score field");

    auto fractionalTick = canonical;
    const auto tick = fractionalTick.find(R"json("tick": 12345)json");
    require(tick != std::string::npos, "Score fixture should expose an exact tick token");
    fractionalTick.replace(tick,
                           std::string_view{R"json("tick": 12345)json"}.size(),
                           R"json("tick": 12345.0000000000001)json");
    requireLoadErrorAt(fractionalTick,
                       "$.timeline.score.clips[0].start.tick",
                       "Score ticks must reject mathematically fractional JSON tokens");

    auto unknownKind = canonical;
    const auto kind = unknownKind.find(R"json("kind": "track")json");
    require(kind != std::string::npos, "Score fixture should expose a track kind");
    unknownKind.replace(
        kind, std::string_view{R"json("kind": "track")json"}.size(), R"json("kind": "bus")json");
    requireLoadErrorAt(unknownKind,
                       "$.timeline.score.tracks[0]",
                       "Score parser should reject unknown closed enum values");

    auto missingField = canonical;
    const auto displayName = missingField.find(R"json(          "displayName": "Voice",
)json");
    require(displayName != std::string::npos, "Score fixture should expose the track display name");
    missingField.erase(displayName,
                       std::string_view{R"json(          "displayName": "Voice",
)json"}
                           .size());
    requireLoadErrorAt(missingField,
                       "$.timeline.score.tracks[0]",
                       "Score parser should reject a missing required field");

    auto oversizedDefinitionId = canonical;
    const auto definitionId = oversizedDefinitionId.find(
        R"json("soundDefinitionId": "voiceDefinition")json");
    require(definitionId != std::string::npos,
            "Score fixture should expose its Sound-definition identity");
    oversizedDefinitionId.replace(
        definitionId,
        std::string_view{
            R"json("soundDefinitionId": "voiceDefinition")json"}
            .size(),
        "\"soundDefinitionId\": \""
            + std::string(
                talas::project::maximumScoreIdentifierBytes + 1,
                'a')
            + "\"");
    requireLoadError(
        oversizedDefinitionId,
        "Score strings should be rejected during bounded JSON decoding");

    auto oversizedTracks = canonical;
    const auto tracks = oversizedTracks.find(R"json("tracks": [)json");
    require(tracks != std::string::npos,
            "Score fixture should expose its track collection");
    const auto insertion = tracks
        + std::string_view{R"json("tracks": [)json"}.size();
    std::string extraTracks;
    for (std::size_t index = 0;
         index < talas::project::maximumScoreTracks + 1;
         ++index)
    {
        extraTracks += "null,";
    }
    oversizedTracks.insert(insertion, extraTracks);
    requireLoadError(
        oversizedTracks,
        "Score collections should be rejected while parsing their first element beyond the fixed limit");

    auto aggregatePoints = canonical;
    std::string pointValues;
    constexpr auto pointsPerLane =
        talas::project::maximumScoreAutomationPoints / 2 + 1;
    for (std::size_t index = 0;
         index < pointsPerLane;
         ++index)
    {
        if (!pointValues.empty())
            pointValues += ',';
        pointValues += "null";
    }
    const auto existingPoints = aggregatePoints.find(
        R"json("points": [)json");
    require(existingPoints != std::string::npos,
            "Score fixture should expose its automation points");
    aggregatePoints.insert(
        existingPoints
            + std::string_view{R"json("points": [)json"}.size(),
        pointValues + ',');
    const auto automation = aggregatePoints.find(
        R"json("automation": [)json");
    require(automation != std::string::npos,
            "Score fixture should expose its automation collection");
    aggregatePoints.insert(
        automation
            + std::string_view{R"json("automation": [)json"}.size(),
        "{\"points\":[" + pointValues + "]},");
    requireLoadError(
        aggregatePoints,
        "automation point arrays should share the aggregate Score point budget during JSON parsing");
}

void testTimelineLoopRoundTripsMigratesAndValidates()
{
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.12345678901234566, 7.9876543210987654};

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, R"json("loop": {)json", "Timeline loop should serialize");
    requireContains(json, R"json("enabled": true)json", "Timeline loop enabled state should serialize");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "serialized Timeline loop should parse");
    require(loaded.document.timeline.loop == document.timeline.loop,
            "Timeline loop should round-trip with exact double precision");

    const auto legacy = talas::project::parseProjectDocument(
        R"json({"format":"talas.project","version":5,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"eventValues":[]},"deviceSettings":{"configured":true,"audioInputDevice":"Input","audioOutputDevice":"Output","sampleRate":48000,"bufferSize":256,"inputChannels":[1],"outputChannels":[1,2],"midiInputDevices":["midi-a"]},"graph":{"nodes":[],"connections":[]}})json");
    require(legacy.ok(), "version-five project should migrate");
    require(legacy.document.formatVersion == talas::project::currentProjectFormatVersion,
            "version-five project should migrate to version eleven");
    require(legacy.document.timeline.loop
                == talas::project::TimelineLoopRegion{false, 0.0, 4.0},
            "legacy projects should receive the disabled default loop");
    require(legacy.document.deviceSettings.configured
                && legacy.document.deviceSettings.sampleRate == 48000.0
                && legacy.document.deviceSettings.bufferSize == 256
                && legacy.document.deviceSettings.midiInputDeviceIdentifiers
                    == std::vector<std::string>{"midi-a"},
            "version-five device settings must survive version-six migration");

    requireLoadError(
        R"json({"format":"talas.project","version":6,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"loop":{"enabled":true,"startSeconds":2,"endSeconds":1},"eventValues":[]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "Timeline loop end should be later than its start");
    requireLoadError(
        R"json({"format":"talas.project","version":6,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"loop":{"enabled":false,"startSeconds":-1,"endSeconds":4},"eventValues":[]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "Timeline loop start should be non-negative even when disabled");
    requireLoadError(
        R"json({"format":"talas.project","version":6,"metadata":{"title":""},"virtualControls":[],"assets":[],"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "version-six Timeline should be required");
    requireLoadError(
        R"json({"format":"talas.project","version":6,"metadata":{"title":""},"virtualControls":[],"assets":[],"timeline":{"eventValues":[]},"deviceSettings":{"configured":false,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":0,"inputChannels":[],"outputChannels":[],"midiInputDevices":[]},"graph":{"nodes":[],"connections":[]}})json",
        "version-six Timeline loop should be required");
}

void testLoadErrors()
{
    requireLoadError(R"json({"format":"wrong","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[]}})json",
                     "wrong format should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":10,"metadata":{"title":""},"graph":{"nodes":[],"connections":[]}})json",
                     "unsupported version should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":"unterminated})json",
                     "malformed JSON should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"typeId":"talas.oscillator"}],"connections":[]}})json",
                     "missing node id should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1"}],"connections":[]}})json",
                     "missing node typeId should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"graph":{"nodes":[],"connections":[]}})json",
                     "missing metadata should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{},"graph":{"nodes":[],"connections":[]}})json",
                     "missing metadata title should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""}})json",
                     "missing graph should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"connections":[]}})json",
                     "missing graph nodes should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[]}})json",
                     "missing graph connections should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[]}})json",
                     "duplicate top-level key should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[{"fromPort":"audioOut","toNode":"out1","toPort":"audioIn"}]}})json",
                     "missing connection fromNode should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[{"fromNode":"osc1","toNode":"out1","toPort":"audioIn"}]}})json",
                     "missing connection fromPort should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[{"fromNode":"osc1","fromPort":"audioOut","toPort":"audioIn"}]}})json",
                     "missing connection toNode should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":[{"fromNode":"osc1","fromPort":"audioOut","toNode":"out1"}]}})json",
                     "missing connection toPort should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","parameters":[{"expression":"440"}]}],"connections":[]}})json",
                     "missing parameter id should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"osc1","typeId":"talas.oscillator","parameters":[{"id":"frequency"}]}],"connections":[]}})json",
                     "missing parameter expression or choice should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"pan1","typeId":"talas.pan","parameters":[{"id":"type","expression":"0","choice":"linear"}]}],"connections":[]}})json",
                     "parameter with expression and choice should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"sample1","typeId":"talas.sample","parameters":[{"id":"setLoop","boolean":"true"}]}],"connections":[]}})json",
                     "parameter boolean should require a JSON boolean");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"sample1","typeId":"talas.sample","parameters":[{"id":"setLoop","expression":"1","boolean":true}]}],"connections":[]}})json",
                     "parameter with expression and boolean should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"sample1","typeId":"talas.sample","parameters":[{"id":"setLoop","choice":"on","boolean":true}]}],"connections":[]}})json",
                     "parameter with choice and boolean should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"sample1","typeId":"talas.sample","parameters":[{"id":"sample","resource":12}]}],"connections":[]}})json",
                     "parameter resource should require a JSON string");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"script1","typeId":"talas.script","parameters":[{"id":"body","text":true}]}],"connections":[]}})json",
                     "parameter text should require a JSON string");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"sample1","typeId":"talas.sample","parameters":[{"id":"sample","expression":"0","resource":"Samples/Kick.aif"}]}],"connections":[]}})json",
                     "parameter with expression and resource should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[{"id":"script1","typeId":"talas.script","parameters":[{"id":"body","boolean":true,"text":"run"}]}],"connections":[]}})json",
                     "parameter with boolean and text should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":{},"connections":[]}})json",
                     "wrong graph nodes type should produce a load error");
    requireLoadError(R"json({"format":"talas.project","version":1,"metadata":{"title":""},"graph":{"nodes":[],"connections":{}}})json",
                     "wrong graph connections type should produce a load error");
}

void testPerformancePresentationV2RoundTripsMacrosAndMeters()
{
    talas::project::ProjectDocument document;
    talas::project::VirtualControl macro;
    macro.eventName = "MacroA";
    macro.minimum = -1.0;
    macro.maximum = 1.0;
    document.virtualControls.push_back(macro);
    talas::project::VirtualControl destination;
    destination.eventName = "Gain";
    document.virtualControls.push_back(destination);
    talas::project::reconcileProjectPresentation(document);

    auto& source = document.presentation.performanceControls[0];
    auto& target = document.presentation.performanceControls[1];
    source.kind = talas::project::ProjectPerformanceControlKind::Macro;
    source.displayName = "Macro A";
    source.position = {96, 128};
    source.modulationSlots.push_back({target.target, 0.75});
    target.kind = talas::project::ProjectPerformanceControlKind::Knob;
    target.position = {320, 128};
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {544, 128}, true});

    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && reopened.document.formatVersion == talas::project::currentProjectFormatVersion
            && reopened.document.presentation.schemaVersion == 2
            && reopened.document.presentation.performanceControls.size() == 2
            && reopened.document.presentation.performanceControls[0].kind
                == talas::project::ProjectPerformanceControlKind::Macro
            && reopened.document.presentation.performanceControls[0]
                    .modulationSlots
                == std::vector<talas::project::ProjectPerformanceModulationSlot>{
                    {target.target, 0.75}}
            && reopened.document.presentation.performanceControls[1].kind
                == talas::project::ProjectPerformanceControlKind::Knob
            && reopened.document.presentation.performanceMeters
                == std::vector<talas::project::ProjectPerformanceMeterPresentation>{
                    {"master-left", 1, {544, 128}, true}},
        "format-20 Performance presentation should round-trip exact macro, "
        "target, depth, visual kind, and output-meter authority");
    require(talas::project::serializeProjectDocument(reopened.document) == canonical,
        "format-20 Performance presentation should serialize byte-stably");

    auto invalidDepth = canonical;
    const auto depth = invalidDepth.find("\"depth\": 0.75");
    require(depth != std::string::npos,
        "invalid-depth fixture should find canonical modulation depth");
    invalidDepth.replace(depth,
        std::string("\"depth\": 0.75").size(),
        "\"depth\": 1.25");
    requireLoadErrorAt(invalidDepth,
        "$.presentation.performanceControls[0].modulationSlots[0]",
        "format-20 parser should reject out-of-range modulation depth");

    auto futurePresentationInLegacyProject = canonical;
    const auto projectVersion = futurePresentationInLegacyProject.find(
        R"json("version": 156)json");
    require(projectVersion != std::string::npos,
        "format-20 fixture should contain its project version");
    futurePresentationInLegacyProject.replace(projectVersion,
        std::string{R"json("version": 156)json"}.size(),
        R"json("version":  19)json");
    requireLoadErrorAt(futurePresentationInLegacyProject,
        "$.presentation",
        "format-19 parser should reject future presentation-v2 members");

    auto cyclic = document;
    auto& first = cyclic.presentation.performanceControls[0];
    auto& second = cyclic.presentation.performanceControls[1];
    second.kind = talas::project::ProjectPerformanceControlKind::Macro;
    second.modulationSlots = {{first.target, 0.5}};
    bool writerRejectedCycle = false;
    try
    {
        static_cast<void>(talas::project::serializeProjectDocument(cyclic));
    }
    catch (const std::invalid_argument&)
    {
        writerRejectedCycle = true;
    }
    require(writerRejectedCycle,
        "format-20 writer should fail closed on cyclic macro routing");

    auto invalidControlId = document;
    invalidControlId.presentation.performanceControls.front().controlId =
        "invalid control id";
    bool writerRejectedInvalidControlId = false;
    try
    {
        static_cast<void>(
            talas::project::serializeProjectDocument(invalidControlId));
    }
    catch (const std::invalid_argument&)
    {
        writerRejectedInvalidControlId = true;
    }
    require(writerRejectedInvalidControlId,
        "format-20 writer should reject a syntactically invalid Performance control identity");

    auto invalidMeterId = document;
    invalidMeterId.presentation.performanceMeters.front().meterId =
        "1-invalid-meter";
    bool writerRejectedInvalidMeterId = false;
    try
    {
        static_cast<void>(
            talas::project::serializeProjectDocument(invalidMeterId));
    }
    catch (const std::invalid_argument&)
    {
        writerRejectedInvalidMeterId = true;
    }
    require(writerRejectedInvalidMeterId,
        "format-20 writer should reject a syntactically invalid Performance meter identity");

    talas::project::reconcileProjectPresentation(invalidControlId);
    talas::project::reconcileProjectPresentation(invalidMeterId);
    require(
        talas::project::isValidProjectPerformanceIdentifier(
            invalidControlId.presentation.performanceControls.front().controlId)
            && invalidMeterId.presentation.performanceMeters.empty(),
        "Performance reconciliation should regenerate invalid control identities and drop invalid meters");

    auto cyclicJson = canonical;
    const auto emptySlots = cyclicJson.find(
        R"json("modulationSlots": [])json");
    require(emptySlots != std::string::npos,
        "format-20 fixture should contain the destination's empty slots");
    cyclicJson.replace(emptySlots,
        std::string{R"json("modulationSlots": [])json"}.size(),
        R"json("modulationSlots": [
          {
            "target": {
              "kind": "eventValue",
              "eventName": "MacroA",
              "domain": "scalar"
            },
            "depth": 0.5
          }
        ])json");
    requireLoadErrorAt(cyclicJson,
        "$.presentation.performanceControls",
        "format-20 parser should fail closed on cyclic macro routing");
}

void testLoadedDocumentMakesValidCoreGraph()
{
    const auto loaded = talas::project::parseProjectDocument(talas::project::serializeProjectDocument(makePatchDocument()));
    require(loaded.ok(), "project JSON should load before graph conversion");

    const auto graph = talas::project::makeGraph(loaded.document);
    require(graph.nodes().size() == 3, "core graph should receive project nodes");
    require(graph.connections().size() == 2, "core graph should receive project connections");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto validation = graph.validate(registry);
    require(validation.ok(), "loaded project graph should validate against built-in node registry");
}

void testCoreGraphMakesProjectDocument()
{
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"out1", "talas.audioOutput"});
    graph.connect({"osc1", "audioOut", "out1", "audioIn"});

    const auto document = talas::project::makeProjectDocument(graph);

    require(document.formatVersion == talas::project::currentProjectFormatVersion, "project document format version should default to the current version");
    require(document.graph.nodes.size() == 2, "project document should copy graph nodes");
    require(document.graph.nodes[0].instanceId == "osc1", "project document should copy node instance ids");
    require(document.graph.nodes[0].typeId == "talas.oscillator", "project document should copy node type ids");
    require(document.graph.nodes[0].parameters.empty(), "project document should not invent parameter expressions from core graph");
    require(document.graph.connections.size() == 1, "project document should copy graph connections");
}
void testFormatThirtySixVoiceProfileRoundTrip()
{
    using Language = talas::expr::ExpressionLanguageVersion;
    auto document = makePatchDocument();
    require(document.formatVersion == talas::project::currentProjectFormatVersion && document.expressionLanguage == Language::VariablesV2,
        "format 37 must preserve the existing default expression language");
    document.expressionLanguage = Language::VoiceV1;
    document.expressionDefinitions.definitions.push_back(
        {"voiceSeed", {}, talas::expr::ValueDomain::Scalar, "?VoiceNumber * 0.1 + 0.001"});
    const auto json = talas::project::serializeProjectDocument(document);
    require(json.find("\"expressionLanguage\": \"voiceV1\"") != std::string::npos,
        "voice expressions must retain a distinct serialized language identity");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok() && loaded.document.formatVersion == talas::project::currentProjectFormatVersion
        && loaded.document.expressionLanguage == Language::VoiceV1
        && loaded.document.expressionDefinitions == document.expressionDefinitions,
        "format 37 must reopen authored voice expressions without specializing the source");
    require(talas::project::serializeProjectDocument(loaded.document) == json,
        "voice profile second serialization must be stable");
    require(talas::project::validateProjectDocumentForPersistence(loaded.document).ok(),
        "voice profile persistence preflight must agree with writer and reader");
    auto oldVersion = json;
    const auto version = oldVersion.find("\"version\": 156");
    require(version != std::string::npos, "voice fixture must declare format 37");
    auto formatThirtySix = oldVersion;
    formatThirtySix.replace(version, std::string{"\"version\": 156"}.size(),
        "\"version\":  36");
    const auto migratedThirtySix =
        talas::project::parseProjectDocument(formatThirtySix);
    require(migratedThirtySix.ok()
            && migratedThirtySix.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedThirtySix.document.expressionLanguage == Language::VoiceV1,
        "format 36 VoiceV1 expressions must migrate without changing language");
    oldVersion.replace(version, std::string{"\"version\": 156"}.size(), "\"version\":  35");
    require(!talas::project::parseProjectDocument(oldVersion).ok(),
        "format 35 must not admit the new voice expression identity");
    document.formatVersion = 35;
    require(!talas::project::validateProjectDocumentForPersistence(document).ok(),
        "old-format in-memory voice profile must fail persistence preflight");
    document = makePatchDocument();
    for (const auto language : {Language::LegacyV1, Language::PureV2, Language::PureV3,
             Language::PureV4, Language::SessionV1, Language::VariablesV1, Language::VariablesV2})
    {
        document.expressionLanguage = language;
        const auto canonical = talas::project::serializeProjectDocument(document);
        const auto reopened = talas::project::parseProjectDocument(canonical);
        require(reopened.ok() && reopened.document.expressionLanguage == language,
            "format 37 must not implicitly upgrade any earlier language profile");
        auto historical = canonical;
        const auto position = historical.find("\"version\": 156");
        require(position != std::string::npos, "legacy-profile fixture must declare current format");
        historical.replace(position, std::string{"\"version\": 156"}.size(), "\"version\":  35");
        const auto migrated = talas::project::parseProjectDocument(historical);
        require(migrated.ok() && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.expressionLanguage == language,
            "format 35 migration must preserve the authored language identity");
    }
}

void testFormatThirtySevenTimedExpressionProfileRoundTrip()
{
    using Language = talas::expr::ExpressionLanguageVersion;
    auto document = makePatchDocument();
    document.expressionLanguage = Language::VariablesV3;
    document.graph.nodes[0].parameters.push_back(
        expressionParameter("amplitude", "1 ramp: 4 ms"));
    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, "\"expressionLanguage\": \"variablesV3\"",
        "timed expressions must persist a distinct language identity");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok()
            && loaded.document.expressionLanguage == Language::VariablesV3
            && loaded.document.graph.nodes[0].parameters[1].expression
                == std::optional<std::string>{"1 ramp: 4 ms"},
        "format 37 must reopen timed expressions without rewriting source");
    require(talas::project::serializeProjectDocument(loaded.document) == json
            && talas::project::validateProjectDocumentForPersistence(
                loaded.document).ok(),
        "timed expression profile must round-trip and pass persistence preflight");
    auto historical = json;
    const auto version = historical.find("\"version\": 156");
    require(version != std::string::npos,
        "timed expression fixture must declare format 37");
    historical.replace(version, std::string{"\"version\": 156"}.size(),
        "\"version\":  36");
    requireLoadErrorAt(historical, "$.expressionLanguage",
        "format 36 must reject VariablesV3 authority");
}

void testFormatThirtyEightPureV5ArrayMappingAuthority()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV5;
    document.graph.nodes.push_back({
        "mapping",
        "talas.constant",
        {expressionParameter("value",
            "intoXArray(0.25, [0, 0.5, 1], [0, 0.2, 1], 0.5)")},
    });
    talas::project::reconcileProjectPresentation(document);
    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, "\"expressionLanguage\": \"pureV5\"",
        "format 38 should persist explicit PureV5 authority");
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok()
            && reopened.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV5,
        "PureV5 mapping should reopen with its exact language profile");

    auto formatThirtyEight = json;
    formatThirtyEight.replace(formatThirtyEight.find("\"version\": 156"),
        std::string_view{"\"version\": 156"}.size(), "\"version\":  38");
    const auto migrated = talas::project::parseProjectDocument(formatThirtyEight);
    require(migrated.ok()
            && migrated.document.expressionLanguage
                == talas::expr::ExpressionLanguageVersion::PureV5,
        "format-38 PureV5 authority must survive later format migrations");

    auto historical = json;
    const auto version = historical.find("\"version\": 156");
    require(version != std::string::npos,
        "PureV5 fixture should declare format 38");
    historical.replace(version, std::string{"\"version\": 156"}.size(),
        "\"version\":  37");
    requireLoadErrorAt(historical, "$.expressionLanguage",
        "format 37 must reject PureV5 authority");
}

void testFormatThirtyNineAnalogSequencerIndexOriginMigration()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "rootSequence", "talas.analogSequencer",
        {{"startIndex", "1 + 1"}, {"endIndex", "3"}},
    });
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "sequenceVoice";
    definition.displayName = "Sequence voice";
    definition.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {{"startIndex", "2"}, {"endIndex", "3"}},
    });
    talas::project::ProjectSoundExposedParameter first;
    first.parameterId = "first";
    first.displayName = "First stage";
    first.defaultValue = {"first", "2"};
    first.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "sequence", "startIndex", {}, talas::expr::ValueDomain::Scalar});
    definition.exposedParameters.push_back(first);
    document.soundDefinitions.push_back(definition);
    talas::project::ProjectNode instance;
    instance.instanceId = "nestedSequence";
    instance.soundDefinitionId = "sequenceVoice";
    instance.parameters.push_back({"first", "3"});
    document.graph.nodes.push_back(instance);
    document.graph.nodes.push_back({"mapper", "talas.midiMapper",
        {choiceParameter("voiceModel", "polyphonicInput")}});

    const auto current = talas::project::serializeProjectDocument(document);
    auto formatThirtyNine = current;
    const auto latestVersion = formatThirtyNine.find("\"version\": 156");
    require(latestVersion != std::string::npos,
        "index migration fixture should declare the latest format");
    formatThirtyNine.replace(latestVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  39");
    const auto retainedThirtyNine =
        talas::project::parseProjectDocument(formatThirtyNine);
    require(retainedThirtyNine.ok()
            && std::ranges::none_of(
                retainedThirtyNine.document.graph.nodes[0].parameters,
                [](const auto& parameter) {
                    return parameter.parameterId == "indexOrigin";
                }),
        "format 39 must retain zero-based AnalogSequencer semantics");

    auto historical = current;
    const auto version = historical.find("\"version\": 156");
    require(version != std::string::npos,
        "index migration fixture should declare the current format");
    historical.replace(version, std::string_view{"\"version\": 156"}.size(),
        "\"version\":  38");
    const auto migrated = talas::project::parseProjectDocument(historical);
    require(migrated.ok(),
        "root and nested format-38 AnalogSequencers should migrate");
    const auto& root = migrated.document.graph.nodes[0];
    const auto& nested = migrated.document.soundDefinitions[0].graph.nodes[0];
    for (const auto* sequence : {&root, &nested})
    {
        const auto origin = std::ranges::find(sequence->parameters,
            "indexOrigin", &talas::project::ProjectParameterExpression::parameterId);
        require(origin != sequence->parameters.end()
                && origin->choice == "legacyOneBased",
            "older root and nested sequences must acquire explicit legacy indexing");
    }
    require(root.parameters[0].expression == "1 + 1"
            && nested.parameters[0].expression == "2"
            && migrated.document.soundDefinitions[0]
                .exposedParameters[0].defaultValue.expression == "2"
            && migrated.document.graph.nodes[1].parameters[0].expression == "3",
        "migration must preserve authored index expressions and exposed overrides");
    require(migrated.document.graph.nodes[2].parameters[0].choice
                == "polyphonicInput",
        "format-38 MIDIMapper voice semantics must survive the index migration");
    const auto canonical = talas::project::serializeProjectDocument(migrated.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document) == canonical,
        "migrated index origin must survive canonical save and reopen");

    const auto newProject = talas::project::parseProjectDocument(current);
    require(newProject.ok()
            && newProject.document.graph.nodes[0].parameters.size() == 2,
        "current-format sequences must retain their default zero-based origin");
    document.graph.nodes[0].parameters.push_back(
        choiceParameter("indexOrigin", "zeroBased"));
    auto premature = talas::project::serializeProjectDocument(document);
    premature.replace(premature.find("\"version\": 156"),
        std::string_view{"\"version\": 156"}.size(), "\"version\":  38");
    requireLoadErrorAt(premature, "$.graph.nodes[0].parameters",
        "format 38 must reject an explicit new index-origin choice");
}

void testAnalogSequencerGeneratedIndexTextRoundTripsWithoutNormalization()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {textParameter("generatedIndex", "  !Stage7  ")},
    });

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, "\"id\": \"generatedIndex\"",
        "GeneratedIndex should serialize as a node parameter");
    requireContains(json, "\"text\": \"  !Stage7  \"",
        "GeneratedIndex should serialize its authored target text unchanged");
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok()
            && reopened.document.formatVersion
                == talas::project::currentProjectFormatVersion
            && reopened.document.graph.nodes[0].parameters[0].text
                == std::optional<std::string>{"  !Stage7  "}
            && talas::project::serializeProjectDocument(reopened.document) == json,
        "GeneratedIndex text should survive current-format save and reopen unchanged");
}

void testFormatFortyOneStepZeroModeMigration()
{
    using Language = talas::expr::ExpressionLanguageVersion;
    talas::project::ProjectDocument document;
    document.expressionLanguage = Language::VariablesV4;
    document.graph.nodes.push_back({
        "rootSequence", "talas.analogSequencer",
        {expressionParameter("step", "0"),
         choiceParameter("indexOrigin", "zeroBased")},
    });

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "sequenceVoice";
    definition.displayName = "Sequence voice";
    definition.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {expressionParameter("step", "1"),
         choiceParameter("indexOrigin", "zeroBased")},
    });
    talas::project::ProjectSoundExposedParameter step;
    step.parameterId = "step";
    step.displayName = "Step";
    step.defaultValue = {"step", "1"};
    step.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "sequence", "step", {}, talas::expr::ValueDomain::Scalar});
    definition.exposedParameters.push_back(step);
    document.soundDefinitions.push_back(definition);

    talas::project::ProjectNode instance;
    instance.instanceId = "nestedSequence";
    instance.soundDefinitionId = "sequenceVoice";
    instance.parameters.push_back(expressionParameter("step", "0"));
    document.graph.nodes.push_back(instance);

    const auto current = talas::project::serializeProjectDocument(document);
    auto formatForty = current;
    const auto currentVersion = formatForty.find("\"version\": 156");
    require(currentVersion != std::string::npos,
        "step-zero fixture should serialize with the current format");
    formatForty.replace(currentVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
    const auto migrated = talas::project::parseProjectDocument(formatForty);
    require(migrated.ok(),
        "format 40 AnalogSequencers should migrate their Step zero behavior");

    const auto requireLegacyAutomatic = [](const auto& sequence) {
        const auto mode = std::ranges::find(
            sequence.parameters,
            "stepZeroMode",
            &talas::project::ProjectParameterExpression::parameterId);
        require(mode != sequence.parameters.end()
                && mode->choice == "legacyAutomatic",
            "every old root or reusable sequence should acquire legacy Step zero behavior");
    };
    requireLegacyAutomatic(migrated.document.graph.nodes.front());
    requireLegacyAutomatic(
        migrated.document.soundDefinitions.front().graph.nodes.front());
    const auto& root = migrated.document.graph.nodes.front();
    const auto rootIndexOrigin = std::ranges::find(
        root.parameters, "indexOrigin",
        &talas::project::ProjectParameterExpression::parameterId);
    const auto& nested = migrated.document.soundDefinitions.front().graph.nodes.front();
    const auto nestedIndexOrigin = std::ranges::find(
        nested.parameters, "indexOrigin",
        &talas::project::ProjectParameterExpression::parameterId);
    require(rootIndexOrigin != root.parameters.end()
                && rootIndexOrigin->choice == "zeroBased"
            && nestedIndexOrigin != nested.parameters.end()
            && nestedIndexOrigin->choice == "zeroBased"
            && migrated.document.expressionLanguage == Language::VariablesV4,
        "format 40 migration should coexist with explicit indexes and VariablesV4 authority");
    require(root.parameters.front().expression == "0"
            && nested.parameters.front().expression == "1"
            && migrated.document.soundDefinitions.front()
                .exposedParameters.front().defaultValue.expression == "1"
            && migrated.document.graph.nodes[1].parameters.front().expression == "0",
        "migration should preserve local Step text, exposed defaults and instance overrides");

    const auto canonical = talas::project::serializeProjectDocument(migrated.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    const auto reopenedMode = [&] {
        const auto& parameters = reopened.document.graph.nodes.front().parameters;
        const auto mode = std::ranges::find(parameters, "stepZeroMode",
            &talas::project::ProjectParameterExpression::parameterId);
        return mode == parameters.end() ? std::optional<std::string>{} : mode->choice;
    };
    require(reopened.ok()
            && reopenedMode() == std::optional<std::string>{"legacyAutomatic"}
            && talas::project::serializeProjectDocument(reopened.document) == canonical,
        "legacy Step-zero authority should survive canonical save and reopen exactly");

    talas::project::ProjectDocument preFormatForty;
    preFormatForty.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {expressionParameter("step", "0")},
    });
    auto formatThirtyEight = talas::project::serializeProjectDocument(preFormatForty);
    const auto latestVersion = formatThirtyEight.find("\"version\": 156");
    require(latestVersion != std::string::npos,
        "pre-40 migration fixture should serialize with format 41");
    formatThirtyEight.replace(latestVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  38");
    const auto migratedThirtyEight =
        talas::project::parseProjectDocument(formatThirtyEight);
    require(migratedThirtyEight.ok(),
        "format 38 should apply both index-origin and Step-zero migrations");
    const auto& oldSequence = migratedThirtyEight.document.graph.nodes.front();
    const auto oldOrigin = std::ranges::find(
        oldSequence.parameters, "indexOrigin",
        &talas::project::ProjectParameterExpression::parameterId);
    const auto oldStepMode = std::ranges::find(
        oldSequence.parameters, "stepZeroMode",
        &talas::project::ProjectParameterExpression::parameterId);
    require(oldOrigin != oldSequence.parameters.end()
                && oldOrigin->choice == "legacyOneBased"
            && oldStepMode != oldSequence.parameters.end()
            && oldStepMode->choice == "legacyAutomatic",
        "format 38 should retain its index migration alongside new compatibility authority");

    auto authoredTooEarlyDocument = document;
    authoredTooEarlyDocument.graph.nodes.front().parameters.push_back(
        choiceParameter("stepZeroMode", "waitForTrigger"));
    auto authoredTooEarly =
        talas::project::serializeProjectDocument(authoredTooEarlyDocument);
    const auto latest = authoredTooEarly.find("\"version\": 156");
    authoredTooEarly.replace(latest,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
    requireLoadErrorAt(authoredTooEarly, "$.graph.nodes[0].parameters",
        "format 40 must reject authored Step-zero choices");

    auto authoredNestedTooEarlyDocument = document;
    authoredNestedTooEarlyDocument.soundDefinitions.front()
        .graph.nodes.front().parameters.push_back(
            choiceParameter("stepZeroMode", "waitForTrigger"));
    auto authoredNestedTooEarly =
        talas::project::serializeProjectDocument(
            authoredNestedTooEarlyDocument);
    const auto nestedVersion = authoredNestedTooEarly.find("\"version\": 156");
    authoredNestedTooEarly.replace(nestedVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
    requireLoadErrorAt(
        authoredNestedTooEarly,
        "$.soundDefinitions[0].graph.nodes[0].parameters",
        "format 40 must reject an authored choice in reusable Sounds too");

    auto exposedTooEarlyDocument = document;
    talas::project::ProjectSoundExposedParameter exposedMode;
    exposedMode.parameterId = "sequenceMode";
    exposedMode.displayName = "Sequence mode";
    exposedMode.kind = talas::core::ParameterKind::Choice;
    exposedMode.choices = {"waitForTrigger", "legacyAutomatic"};
    exposedMode.defaultValue =
        choiceParameter("sequenceMode", "waitForTrigger");
    exposedMode.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "sequence", "stepZeroMode", {}, talas::expr::ValueDomain::Scalar});
    exposedTooEarlyDocument.soundDefinitions.front().exposedParameters
        .push_back(std::move(exposedMode));
    auto exposedTooEarly = talas::project::serializeProjectDocument(
        exposedTooEarlyDocument);
    const auto exposedVersion = exposedTooEarly.find("\"version\": 156");
    exposedTooEarly.replace(exposedVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
    requireLoadErrorAt(
        exposedTooEarly,
        "$.soundDefinitions[0].parameters[1].bindings[0]",
        "format 40 must reject an exposed binding that can override Step-zero compatibility");

    talas::project::ProjectDocument explicitCurrent;
    explicitCurrent.graph.nodes.push_back({
        "sequence", "talas.analogSequencer",
        {choiceParameter("stepZeroMode", "waitForTrigger")},
    });
    const auto explicitJson = talas::project::serializeProjectDocument(explicitCurrent);
    const auto explicitReopened = talas::project::parseProjectDocument(explicitJson);
    require(explicitReopened.ok()
            && explicitReopened.document.graph.nodes.front().parameters.front().choice
                == "waitForTrigger"
            && talas::project::serializeProjectDocument(explicitReopened.document)
                == explicitJson,
        "format 41 should preserve an explicitly authored Step-zero choice");

    auto future = explicitJson;
    const auto version = future.find("\"version\": 156");
    future.replace(version,
        std::string_view{"\"version\": 156"}.size(), "\"version\": 157");
    requireLoadErrorAt(future, "$.version",
        "format 71 should remain outside the supported migration boundary");
}

void testOlderFormatsRejectNewerLiveSetAndBindingSyntax()
{
    const auto withVersion = [](std::string json, int version) {
        const auto current = json.find("\"version\": 156");
        require(current != std::string::npos,
            "fixture should serialize with format 41");
        json.replace(current, std::string_view{"\"version\": 156"}.size(),
            "\"version\": " + std::to_string(version));
        return json;
    };
    const auto exposedChoiceBinding = [](
        std::string parameterId,
        std::vector<std::string> choices,
        std::string nodeId,
        std::string nodeParameterId) {
        talas::project::ProjectSoundExposedParameter exposed;
        exposed.parameterId = parameterId;
        exposed.displayName = parameterId;
        exposed.kind = talas::core::ParameterKind::Choice;
        exposed.defaultValue = choiceParameter(parameterId, choices.front());
        exposed.choices = std::move(choices);
        exposed.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            std::move(nodeId), std::move(nodeParameterId), {},
            talas::expr::ValueDomain::Scalar});
        return exposed;
    };

    talas::project::ProjectDocument originDocument;
    talas::project::ProjectSoundDefinition sequenceVoice;
    sequenceVoice.definitionId = "sequenceVoice";
    sequenceVoice.displayName = "Sequence voice";
    sequenceVoice.graph.nodes.push_back({"sequence", "talas.analogSequencer", {}});
    sequenceVoice.exposedParameters.push_back(exposedChoiceBinding(
        "sequenceOrigin", {"zeroBased", "legacyOneBased"},
        "sequence", "indexOrigin"));
    originDocument.soundDefinitions.push_back(std::move(sequenceVoice));
    const auto originJson =
        talas::project::serializeProjectDocument(originDocument);
    require(talas::project::parseProjectDocument(originJson).ok(),
        "format 41 should accept an exposed indexOrigin binding");
    requireLoadErrorAt(withVersion(originJson, 38),
        "$.soundDefinitions[0].parameters[0].bindings[0]",
        "format 38 must reject a binding that can override the index migration");

    talas::project::ProjectDocument voiceDocument;
    talas::project::ProjectSoundDefinition mapperVoice;
    mapperVoice.definitionId = "mapperVoice";
    mapperVoice.displayName = "Mapper voice";
    mapperVoice.graph.nodes.push_back({"mapper", "talas.midiMapper", {}});
    mapperVoice.exposedParameters.push_back(exposedChoiceBinding(
        "mapperModel", {"polyphonicInput", "legacyPassthrough"},
        "mapper", "voiceModel"));
    voiceDocument.soundDefinitions.push_back(std::move(mapperVoice));
    const auto voiceJson =
        talas::project::serializeProjectDocument(voiceDocument);
    require(talas::project::parseProjectDocument(voiceJson).ok(),
        "format 41 should accept an exposed voiceModel binding");
    requireLoadErrorAt(withVersion(voiceJson, 37),
        "$.soundDefinitions[0].parameters[0].bindings[0]",
        "format 37 must reject a binding that can override the voice migration");

    talas::project::ProjectDocument muteDocument;
    muteDocument.soundDefinitions.push_back({"voice", "Voice"});
    talas::project::ProjectLiveSet parallel;
    parallel.liveSetId = "parallel";
    parallel.displayName = "Parallel";
    talas::project::ProjectLiveSetCell sound;
    sound.cellId = "sound";
    sound.displayName = "Sound";
    sound.target = talas::project::ProjectLiveSetSoundTarget{"voice", {}};
    auto mute = sound;
    mute.cellId = "mute";
    mute.displayName = "Mute";
    mute.row = 1;
    mute.target = talas::project::ProjectLiveSetTrackMuteTarget{true};
    parallel.cells = {sound, mute};
    muteDocument.liveSetLibrary.liveSets.push_back(std::move(parallel));
    const auto muteJson = talas::project::serializeProjectDocument(muteDocument);
    require(talas::project::parseProjectDocument(withVersion(muteJson, 37)).ok(),
        "format 37 introduced track mute cells");
    requireLoadErrorAt(withVersion(muteJson, 36),
        "$.liveSetLibrary.liveSets[0].cells[1].target",
        "format 36 must reject track mute cells");

    talas::project::ProjectDocument routeDocument;
    talas::project::ProjectLiveSet routed;
    routed.liveSetId = "routes";
    routed.displayName = "Routes";
    for (std::uint32_t column = 0; column < 2; ++column)
    {
        const auto suffix = std::to_string(column);
        talas::project::ProjectSoundDefinition effect;
        effect.definitionId = "effect" + suffix;
        effect.displayName = effect.definitionId;
        effect.inputs.push_back({
            "audioIn", talas::core::PortType::Audio, "vca", "audioIn"});
        effect.outputs.push_back({
            "audioOut", talas::core::PortType::Audio, "vca", "audioOut"});
        effect.graph.nodes.push_back({"vca", "talas.vca", {}});
        routeDocument.soundDefinitions.push_back(std::move(effect));
        routeDocument.graph.nodes.push_back(
            {"input" + suffix, "talas.audioInput", {}});
        routeDocument.graph.nodes.push_back(
            {"instance" + suffix, "", {}, "effect" + suffix});
        routeDocument.graph.connections.push_back({
            "input" + suffix, "audioOut", "instance" + suffix, "audioIn"});
        routed.cells.push_back({
            "cell" + suffix, "Cell " + suffix, 0, column, {},
            talas::project::ProjectLiveSetSoundTarget{
                "effect" + suffix, "instance" + suffix}, {}});
    }
    routed.routes = {{0, 1}};
    routeDocument.liveSetLibrary.liveSets.push_back(std::move(routed));
    const auto routeJson = talas::project::serializeProjectDocument(routeDocument);
    require(talas::project::parseProjectDocument(withVersion(routeJson, 37)).ok(),
        "format 37 introduced live-set routes");
    requireLoadErrorAt(withVersion(routeJson, 36),
        "$.liveSetLibrary.liveSets[0].routes",
        "format 36 must reject live-set routes");
}

std::string withProjectFormat(std::string json, int version)
{
    const auto current = json.find("\"version\": 156");
    require(current != std::string::npos,
        "fixture should serialize with the current format");
    json.replace(current, std::string_view{"\"version\": 156"}.size(),
        "\"version\": " + std::to_string(version));
    return json;
}

bool hasConnection(
    const talas::project::ProjectGraph& graph,
    const talas::core::Connection& connection)
{
    return std::ranges::find(graph.connections, connection)
        != graph.connections.end();
}

const talas::project::ProjectNode* findNode(
    const talas::project::ProjectGraph& graph,
    std::string_view id)
{
    const auto found = std::ranges::find(
        graph.nodes, id, &talas::project::ProjectNode::instanceId);
    return found == graph.nodes.end() ? nullptr : &*found;
}

bool isVca(
    const talas::project::ProjectGraph& graph,
    std::string_view id,
    std::string_view gain)
{
    const auto* vca = findNode(graph, id);
    return vca != nullptr && vca->typeId == "talas.vca"
        && vca->parameters.size() == 1
        && vca->parameters.front().parameterId == "gain"
        && vca->parameters.front().expression
            == std::optional<std::string>{std::string{gain}};
}

// Format 43 replaces the two-input Mixer with the Talas Mixer: one Inputs port
// adding every connection, and Left and Right levels. A gain other than a
// plain unit gain moves to a VCA on its input, with whatever refers to it.
void testFormatFortyThreeMigratesTwoInputMixersToTalasMixer()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"a", "talas.oscillator", {}},
        {"b", "talas.oscillator", {}},
        {"lfo", "talas.lfo", {}},
        {"vca1", "talas.vca", {}},
        {"mix", "talas.mixer", {{"gain1", "1.0"}, {"gain2", "0.5"}}},
        {"mix2", "talas.mixer", {}},
        {"mix3", "talas.mixer", {}},
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"b", "audioOut", "vca1", "audioIn"},
        {"a", "audioOut", "mix", "in1"},
        {"vca1", "audioOut", "mix", "in2"},
        {"a", "audioOut", "mix2", "in1"},
        {"lfo", "value", "mix2", "gain1"},
        {"a", "audioOut", "mix2", "in2"},
        {"a", "audioOut", "mix3", "in1"},
        {"a", "audioOut", "mix3", "in2"},
        {"mix", "audioOut", "out", "audioIn"},
        {"mix2", "audioOut", "out", "rightIn"},
    };

    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.inputs = {
        {"source", talas::core::PortType::Audio, "m", "in1"},
        {"level", talas::core::PortType::Control, "m", "gain2"},
    };
    voice.outputs = {{"audioOut", talas::core::PortType::Audio, "m", "audioOut"}};
    voice.graph.nodes = {
        {"osc", "talas.oscillator", {}},
        {"m", "talas.mixer", {{"gain1", "0.25"}}},
    };
    voice.graph.connections = {{"osc", "audioOut", "m", "in2"}};
    talas::project::ProjectSoundExposedParameter level;
    level.parameterId = "level1";
    level.displayName = "Level 1";
    level.defaultValue.parameterId = "level1";
    level.defaultValue.expression = "0.25";
    level.bindings.push_back({
        talas::project::ProjectSoundParameterTarget::NodeParameter,
        "m",
        "gain1",
        {},
        talas::expr::ValueDomain::Scalar,
    });
    voice.exposedParameters.push_back(std::move(level));
    document.soundDefinitions.push_back(std::move(voice));

    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "mixes";
    bank.displayName = "Mixes";
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "quiet";
    snapshot.displayName = "Quiet";
    talas::project::SnapshotValueEntry entry;
    entry.target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    entry.target.nodeId = "mix";
    entry.target.parameterId = "gain2";
    entry.value.number = 0.125;
    snapshot.values.push_back(entry);
    bank.snapshots.push_back(std::move(snapshot));
    document.snapshotLibrary.banks.push_back(std::move(bank));

    const auto current = talas::project::serializeProjectDocument(document);
    const auto unchanged = talas::project::parseProjectDocument(current);
    require(unchanged.ok()
            && findNode(unchanged.document.graph, "mix")->typeId == "talas.mixer",
        "a current project should keep its two-input Mixer");

    const auto loaded = talas::project::parseProjectDocument(
        withProjectFormat(current, 42));
    require(loaded.ok(), "a format 42 project with Mixers should open");
    const auto& root = loaded.document.graph;
    const auto* mix = findNode(root, "mix");
    require(mix != nullptr && mix->typeId == "talas.mixerV2"
            && mix->parameters.size() == 2
            && mix->parameters[0].parameterId == "left"
            && mix->parameters[0].expression == std::optional<std::string>{"1.0"}
            && mix->parameters[1].parameterId == "right"
            && mix->parameters[1].expression == std::optional<std::string>{"1.0"},
        "a two-input Mixer should become the Talas Mixer at unit levels");
    require(hasConnection(root, {"a", "audioOut", "mix", "inputs"})
            && isVca(root, "vca2", "0.5")
            && hasConnection(root, {"vca1", "audioOut", "vca2", "audioIn"})
            && hasConnection(root, {"vca2", "audioOut", "mix", "inputs"})
            && hasConnection(root, {"mix", "leftOut", "out", "audioIn"}),
        "a unit gain should feed Inputs directly and another gain through a new VCA");
    require(isVca(root, "vca3", "1.0")
            && hasConnection(root, {"a", "audioOut", "vca3", "audioIn"})
            && hasConnection(root, {"lfo", "value", "vca3", "gain"})
            && hasConnection(root, {"vca3", "audioOut", "mix2", "inputs"})
            && hasConnection(root, {"a", "audioOut", "mix2", "inputs"})
            && hasConnection(root, {"mix2", "leftOut", "out", "rightIn"}),
        "a connected gain should control the VCA on its input");
    require(hasConnection(root, {"a", "audioOut", "mix3", "inputs"})
            && isVca(root, "vca4", "1.0")
            && hasConnection(root, {"a", "audioOut", "vca4", "audioIn"})
            && hasConnection(root, {"vca4", "audioOut", "mix3", "inputs"}),
        "a Sound feeding both inputs should still be added twice");
    require(std::ranges::none_of(root.connections, [](const auto& connection) {
                return connection.toPort == "in1" || connection.toPort == "in2"
                    || connection.toPort == "gain1" || connection.toPort == "gain2"
                    || (connection.fromPort == "audioOut"
                        && connection.fromNode.starts_with("mix"));
            }),
        "no connection should keep a two-input Mixer port");
    for (const auto* id : {"vca2", "vca3", "vca4"})
    {
        require(talas::project::findProjectNodePresentation(
                    loaded.document, std::nullopt, id) != nullptr,
            std::string{"migrated "} + id + " should be placed on the canvas");
    }
    const auto& target = loaded.document.snapshotLibrary.banks.front()
        .snapshots.front().values.front().target;
    require(target.nodeId == "vca2" && target.parameterId == "gain",
        "a snapshot of a moved gain should follow it to its VCA");

    const auto& definition = loaded.document.soundDefinitions.front();
    require(findNode(definition.graph, "m")->typeId == "talas.mixerV2"
            && isVca(definition.graph, "vca1", "0.25")
            && isVca(definition.graph, "vca2", "1.0")
            && hasConnection(definition.graph, {"vca1", "audioOut", "m", "inputs"})
            && hasConnection(definition.graph, {"osc", "audioOut", "vca2", "audioIn"})
            && hasConnection(definition.graph, {"vca2", "audioOut", "m", "inputs"}),
        "a Sound definition's Mixer should migrate inside its own graph");
    require(definition.inputs
                == std::vector<talas::project::ProjectSoundPort>{
                    {"source", talas::core::PortType::Audio, "vca1", "audioIn"},
                    {"level", talas::core::PortType::Control, "vca2", "gain"}}
            && definition.outputs
                == std::vector<talas::project::ProjectSoundPort>{
                    {"audioOut", talas::core::PortType::Audio, "m", "leftOut"}},
        "Sound ports should follow the Mixer's inputs and output");
    const auto& binding = definition.exposedParameters.front().bindings.front();
    require(binding.nodeId == "vca1" && binding.parameterId == "gain",
        "a bound gain should follow its input to the VCA");

    require(talas::project::validateProjectDocumentForPersistence(
                loaded.document).ok(),
        "the migrated project should pass persistence preflight");
    const auto saved = talas::project::serializeProjectDocument(loaded.document);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document) == saved,
        "the migrated project should save and reopen canonically");

    // Without the 16-bit caches it migrated to (format 154), which a
    // format-42 project has without saying so.
    auto unsized = loaded.document;
    unsized.timeline.cacheWordSize = 24;
    requireLoadErrorAt(withProjectFormat(talas::project::serializeProjectDocument(unsized), 42),
        "$.graph.nodes[4].typeId",
        "format 42 must reject the Talas Mixer");
}

void testFormatFortyTwoValProfileRoundTripsAndRejectsFormatFortyOne()
{
    using Language = talas::expr::ExpressionLanguageVersion;
    auto document = makePatchDocument();
    document.expressionLanguage = Language::ValV1;
    document.graph.nodes[0].parameters[0].expression = "4 c hz * 2";

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, "\"version\": 156",
        "ValV1 projects should use the current portable format");
    requireContains(json, "\"expressionLanguage\": \"valV1\"",
        "ValV1 authority should be explicit in the project");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok()
            && loaded.document.expressionLanguage == Language::ValV1
            && loaded.document.graph.nodes[0].parameters[0].expression
                == std::optional<std::string>{"4 c hz * 2"},
        "ValV1 expression source bytes should reopen");
    require(talas::project::serializeProjectDocument(loaded.document) == json
            && talas::project::validateProjectDocumentForPersistence(
                loaded.document).ok(),
        "ValV1 should round-trip canonically and pass persistence preflight");

    auto historical = json;
    const auto version = historical.find("\"version\": 156");
    historical.replace(version, std::string_view{"\"version\": 156"}.size(),
        "\"version\":  41");
    requireLoadErrorAt(historical, "$.expressionLanguage",
        "format 41 must reject ValV1 authority");
}

void testFormatFortyVariablesV4ProfileRoundTripsAndRejectsFormatThirtyNine()
{
    using Language = talas::expr::ExpressionLanguageVersion;
    auto document = makePatchDocument();
    document.expressionLanguage = Language::VariablesV4;
    document.variableLibrary.variables = {
        {"Level", "Level", talas::expr::ValueDomain::Scalar, 0.75},
    };
    document.graph.nodes[1].parameters[0].expression = "!Level db";

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(json, "\"version\": 156",
        "VariablesV4 projects should use the current portable format");
    requireContains(json, "\"expressionLanguage\": \"variablesV4\"",
        "VariablesV4 authority should be explicit in the project");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok()
            && loaded.document.formatVersion
                == talas::project::currentProjectFormatVersion
            && loaded.document.expressionLanguage == Language::VariablesV4
            && loaded.document.graph.nodes[1].parameters[0].expression
                == std::optional<std::string>{"!Level db"},
        "VariablesV4 expression source bytes should reopen");
    require(talas::project::serializeProjectDocument(loaded.document) == json
            && talas::project::validateProjectDocumentForPersistence(
                loaded.document).ok(),
        "VariablesV4 should round-trip canonically and pass persistence preflight");

    auto formatForty = json;
    const auto latestVersion = formatForty.find("\"version\": 156");
    formatForty.replace(latestVersion,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  40");
    const auto loadedForty = talas::project::parseProjectDocument(formatForty);
    require(loadedForty.ok()
            && loadedForty.document.expressionLanguage == Language::VariablesV4,
        "format 40 should admit the VariablesV4 profile introduced there");

    auto historical = json;
    const auto version = historical.find("\"version\": 156");
    require(version != std::string::npos,
        "VariablesV4 fixture should declare format 40");
    historical.replace(version, std::string_view{"\"version\": 156"}.size(),
        "\"version\":  39");
    requireLoadErrorAt(historical, "$.expressionLanguage",
        "format 39 must reject VariablesV4 authority");

    document.formatVersion = 39;
    const auto oldFormatValidation =
        talas::project::validateProjectDocumentForPersistence(document);
    require(std::ranges::any_of(
                oldFormatValidation.diagnostics,
                [](const auto& diagnostic) {
                    return diagnostic.code
                            == talas::project::ProjectPersistenceValidationCode::InvalidExpressionLanguage
                        && diagnostic.path == "$.expressionLanguage";
                }),
        "format-39 in-memory documents must reject VariablesV4 persistence");

    auto timedV3 = makePatchDocument();
    timedV3.expressionLanguage = Language::VariablesV3;
    timedV3.graph.nodes[0].parameters.push_back(
        expressionParameter("amplitude", "1 ramp: 4 ms"));
    auto formatThirtyNine = talas::project::serializeProjectDocument(timedV3);
    const auto latest = formatThirtyNine.find("\"version\": 156");
    require(latest != std::string::npos,
        "format-39 migration fixture should declare format 40 before downgrade");
    formatThirtyNine.replace(latest,
        std::string_view{"\"version\": 156"}.size(), "\"version\":  39");
    const auto migrated = talas::project::parseProjectDocument(formatThirtyNine);
    require(migrated.ok()
            && migrated.document.formatVersion
                == talas::project::currentProjectFormatVersion
            && migrated.document.expressionLanguage == Language::VariablesV3
            && migrated.document.graph.nodes[0].parameters[1].expression
                == std::optional<std::string>{"1 ramp: 4 ms"},
        "format 39 migration should preserve the earlier profile and expression bytes");
}


// Recursive-descent parsing consumes stack per nesting level. A crafted
// project must produce a diagnostic, not a stack overflow (the same parser
// serves recovery, session and recent-project files on every launch).
void testDeeplyNestedJsonFailsClosedInsteadOfOverflowingTheStack()
{
    constexpr std::size_t depth = 200000;
    std::string json = R"({"format":"talas.project","version":1,"x-extension":)";
    json += std::string(depth, '[') + std::string(depth, ']') + "}";
    const auto loaded = talas::project::parseProjectDocument(json);
    require(!loaded.ok() && !loaded.errors.empty(),
            "deeply nested JSON must be rejected with a diagnostic");
}
}

// The project keeps its timecode frame rate, 24, 25
// or 30, from format 68, and writes it only when it is not 30.
void testFormatSixtyEightTimecodeFramesPerSecond()
{
    require(talas::project::serializeProjectDocument(talas::project::ProjectDocument{})
                .find("timecodeFramesPerSecond") == std::string::npos,
        "the default timecode frame rate should not be written");
    talas::project::ProjectDocument document;
    document.expressionDefinitions.timecodeFramesPerSecond = 25;
    const auto json = talas::project::serializeProjectDocument(document);
    require(json.find("\"timecodeFramesPerSecond\": 25") != std::string::npos,
        "a timecode frame rate of 25 should be written");
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok() && reopened.document.expressionDefinitions.timecodeFramesPerSecond == 25,
        "a timecode frame rate of 25 should reopen");
    auto older = json;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(),
        "\"version\":  67");
    require(!talas::project::parseProjectDocument(older).ok(),
        "format 67 should refuse a timecode frame rate");
    auto unknown = json;
    unknown.replace(unknown.find("\"timecodeFramesPerSecond\": 25"),
        std::string_view{"\"timecodeFramesPerSecond\": 25"}.size(),
        "\"timecodeFramesPerSecond\": 29");
    require(!talas::project::parseProjectDocument(unknown).ok(),
        "a timecode frame rate other than 24, 25 or 30 should be refused");
}

// Format 156: the project keeps how its
// fields read rounded and truncated, a beat and a number given to
// inSOSPitch, each written only when it is not the default; a new project
// keeps a beat at a quarter note = 60, and a file before 156 naming any of
// them is refused, older projects keeping the defaults.
void testFormatOneHundredFiftySixFieldReadingSettings()
{
    const auto plain = talas::project::serializeProjectDocument(talas::project::ProjectDocument{});
    require(plain.find("roundingRule") == std::string::npos && plain.find("fieldBeatTempoBpm") == std::string::npos
            && plain.find("unitlessSOSPitch") == std::string::npos,
        "the default field reading settings should not be written");
    const auto fresh = talas::project::makeNewProjectDocument();
    require(fresh.expressionDefinitions.fieldBeatTempoBpm == std::optional<double>{60.0}
            && fresh.expressionDefinitions.rounding == talas::expr::RoundingRule::Smalltalk
            && fresh.expressionDefinitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Hertz
            && fresh.timeline.musicalTime.tempo.size() == 1
            && fresh.timeline.musicalTime.tempo.front().microsecondsPerQuarter == 500'000,
        "a new project's beat should be a quarter note at 60, its Timeline keeping its own tempo");
    talas::project::ProjectDocument document;
    document.expressionDefinitions.rounding = talas::expr::RoundingRule::ManualTable;
    document.expressionDefinitions.fieldBeatTempoBpm = 60.0;
    document.expressionDefinitions.unitlessSOSPitch = talas::expr::UnitlessSOSPitch::Linear;
    const auto json = talas::project::serializeProjectDocument(document);
    require(json.find("\"roundingRule\": \"manualTable\"") != std::string::npos
            && json.find("\"fieldBeatTempoBpm\": 60") != std::string::npos
            && json.find("\"unitlessSOSPitch\": \"linear\"") != std::string::npos,
        "the chosen field reading settings should be written");
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok() && reopened.document.expressionDefinitions == document.expressionDefinitions,
        "the field reading settings should reopen");
    require(talas::project::validateProjectDocumentForPersistence(document).ok(),
        "the chosen field reading settings should be valid to save");
    auto older = json;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155");
    const auto refused = talas::project::parseProjectDocument(older);
    require(!refused.ok() && refused.errors.size() == 3, "format 155 should refuse each field reading setting");
    const auto previous = talas::project::parseProjectDocument(std::string{plain}.replace(
        plain.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155"));
    require(previous.ok() && previous.document.expressionDefinitions.rounding == talas::expr::RoundingRule::Smalltalk
            && !previous.document.expressionDefinitions.fieldBeatTempoBpm.has_value()
            && previous.document.expressionDefinitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Hertz,
        "a project of format 155 should keep Smalltalk's rounding, the Timeline's beat and hertz");
    for (const auto& [from, to] : {std::pair{std::string_view{"\"fieldBeatTempoBpm\": 60"}, std::string_view{"\"fieldBeatTempoBpm\": 0"}},
             std::pair{std::string_view{"\"fieldBeatTempoBpm\": 60"}, std::string_view{"\"fieldBeatTempoBpm\": 1001"}},
             std::pair{std::string_view{"\"roundingRule\": \"manualTable\""}, std::string_view{"\"roundingRule\": \"smalltalk\""}},
             std::pair{std::string_view{"\"unitlessSOSPitch\": \"linear\""}, std::string_view{"\"unitlessSOSPitch\": 1"}}})
    {
        auto wrong = json;
        wrong.replace(wrong.find(from), from.size(), to);
        require(!talas::project::parseProjectDocument(wrong).ok(), "the setting " + std::string{to} + " should be refused");
    }
    auto tooSlow = document;
    tooSlow.expressionDefinitions.fieldBeatTempoBpm = 0.5;
    require(!talas::project::validateProjectDocumentForPersistence(tooSlow).ok(),
        "a beat tempo below 1 should not be saved");
}

// Format 156: three choices, off by default and written only when on,
// refused in an older project, which loads with them off and plays as it
// did.
void testFormatOneHundredFiftySixDocumentedChoicesRoundTrip()
{
    constexpr std::array names{"pastedSoundsSaturateAtUnity", "pastedSoundsReadThePreviousMillisecond",
        "inverseOfZeroIsInfinite"};
    const auto canonical = talas::project::serializeProjectDocument(talas::project::ProjectDocument{});
    for (const auto* name : names)
        require(canonical.find(name) == std::string::npos, std::string{name} + " should not be written while off");
    talas::project::ProjectDocument document;
    document.pastedSoundsSaturateAtUnity = true;
    document.pastedSoundsReadThePreviousMillisecond = true;
    document.inverseOfZeroIsInfinite = true;
    const auto json = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(json);
    require(reopened.ok() && reopened.document.pastedSoundsSaturateAtUnity
            && reopened.document.pastedSoundsReadThePreviousMillisecond && reopened.document.inverseOfZeroIsInfinite,
        "the format-156 choices should round-trip");
    for (const auto* name : names)
    {
        talas::project::ProjectDocument one;
        one.pastedSoundsSaturateAtUnity = std::string_view{name} == "pastedSoundsSaturateAtUnity";
        one.pastedSoundsReadThePreviousMillisecond = std::string_view{name} == "pastedSoundsReadThePreviousMillisecond";
        one.inverseOfZeroIsInfinite = std::string_view{name} == "inverseOfZeroIsInfinite";
        auto older = talas::project::serializeProjectDocument(one);
        require(older.find(std::string{"\""} + name + "\": true") != std::string::npos, std::string{name} + " should be written");
        older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155");
        require(!talas::project::parseProjectDocument(older).ok(), std::string{name} + " should require format 156");
    }
    auto olderCanonical = canonical;
    olderCanonical.replace(olderCanonical.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 155");
    const auto olderLoaded = talas::project::parseProjectDocument(olderCanonical);
    require(olderLoaded.ok() && !olderLoaded.document.pastedSoundsSaturateAtUnity
            && !olderLoaded.document.pastedSoundsReadThePreviousMillisecond && !olderLoaded.document.inverseOfZeroIsInfinite,
        "a format-155 project should load with the choices off");
}

int main()
{
    testDeeplyNestedJsonFailsClosedInsteadOfOverflowingTheStack();
    testFormatSixtyEightTimecodeFramesPerSecond();
    testFormatOneHundredFiftySixFieldReadingSettings();
    testFormatOneHundredFiftySixDocumentedChoicesRoundTrip();
    testFormatFortyVariablesV4ProfileRoundTripsAndRejectsFormatThirtyNine();
    testFormatFortyTwoValProfileRoundTripsAndRejectsFormatFortyOne();
    testFormatFortyThreeMigratesTwoInputMixersToTalasMixer();
    testAnalogSequencerGeneratedIndexTextRoundTripsWithoutNormalization();
    testFormatThirtyNineAnalogSequencerIndexOriginMigration();
    testFormatFortyOneStepZeroModeMigration();
    testOlderFormatsRejectNewerLiveSetAndBindingSyntax();
    testFormatThirtyEightPureV5ArrayMappingAuthority();
    testFormatThirtySevenTimedExpressionProfileRoundTrip();
    testFormatThirtySixVoiceProfileRoundTrip();
    testEmptyProjectSerializesToCanonicalJson();
    testFormatTwentySevenOwnsVariableExpressions();
    testFormatTwelveAddsEmptySampleMapLibrary();
    testFormatThirteenPreservedComponentRoundTripsCanonically();
    testFormatTwelveMigratesWithoutPreservedComponents();
    testFormatElevenPopulatedSnapshotBankRoundTrips();
    testAllSnapshotScopesTargetsAndValuesRoundTripCanonically();
    testGraphRoundTripsThroughJson();
    testHistoricalPackedFftJsonIdentityRoundTripsUnchanged();
    testSoundDefinitionsRoundTripThroughCurrentJson();
    testSoundInterfacesAndSparseOverridesRoundTripThroughVersionNineJson();
    testEverySoundInterfaceParameterKindRoundTripsCanonically();
    testVersionNineSoundInterfaceParsingRejectsInvalidSchema();
    testVersionEightSoundDefinitionsMigrateToInterfaceVersionOne();
    testSoundInstancesRoundTripWithExplicitDefinitionIdentity();
    testNodeParsingRequiresExactlyOneTypeIdentity();
    testEverySoundBoundaryPortTypeRoundTrips();
    testComplexSpectrumSoundBoundaryUsesFormatTen();
    testVersionSevenMigratesToEmptyCurrentSoundDefinitions();
    testSoundDefinitionParsingRejectsInvalidModels();
    testVersionOneRampMigratesToLinearRamp();
    testVersionTwoRampIdentityRoundTripsWithoutMigration();
    testVersionThreeTimelineMigratesToVersionFourWithoutAssets();
    testLegacyVersionsIgnoreFutureAssetFields();
    testSerializedNodesUseTypeIdOnly();
    testStringEscapingRoundTrips();
    testChoiceParameterRoundTripsThroughJson();
    testBooleanParameterRoundTripsThroughJson();
    testResourceAndTextParametersRoundTripThroughJson();
    testMidiMapperModesRoundTripAndBlankMigratesCanonically();
    testSerializerPreservesInvalidExpressionAndChoiceParameter();
    testSerializerPreservesInvalidExpressionBooleanAndChoiceBooleanParameters();
    testSerializerPreservesInvalidResourceAndTextMixedParameters();
    testUnknownObjectFieldsAreIgnored();
    testNodeCanvasPositionsRoundTripAndValidate();
    testVersionTenPositionsMigrateToFormatElevenStateLayers();
    testLegacyNestedPositionsMigrateToScopedPresentation();
    testFormatElevenWriterRequiresCompletePerformanceIdentity();
    testFormatElevenWriterCanonicalizesIdentityKeyedPresentation();
    testVirtualControlMetadataRoundTrips();
    testFormatFourteenMidiSourceAuthorityRoundTripsAndMigrates();
    testVirtualControlParsingRejectsInvalidModels();
    testProjectAssetsRoundTripAndValidate();
    testTimelineEventValuesRoundTripAndValidate();
    testFormatTwentyOneScoreRoundTripsAndRejectsMalformedAuthority();
    testTimelineLoopRoundTripsMigratesAndValidates();
    testLoadErrors();
    testPerformancePresentationV2RoundTripsMacrosAndMeters();
    testLoadedDocumentMakesValidCoreGraph();
    testCoreGraphMakesProjectDocument();
    return 0;
}
