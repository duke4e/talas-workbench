#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSampleMaps.h"
#include "talas/project/ProjectValidation.h"

#include <ranges>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef TALAS_SOURCE_DIR
#error "TALAS_SOURCE_DIR must identify the repository root"
#endif

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(
    std::string_view source,
    std::string_view expected,
    const std::string& message)
{
    require(source.find(expected) != std::string_view::npos, message);
}

void requireLoadErrorAt(
    std::string_view source,
    std::string_view path,
    const std::string& message)
{
    const auto loaded = talas::project::parseProjectDocument(source);
    require(!loaded.ok(), message);
    require(
        std::ranges::any_of(
            loaded.errors,
            [&](const auto& error) { return error.path == path; }),
        message + " should report " + std::string(path));
}

void downgradeEmptyPresentationToV1(std::string& source)
{
    const auto presentation = source.find("  \"presentation\": {\n");
    const auto version = source.find(
        "    \"version\": 2,\n", presentation);
    const std::string controls =
        "    \"performanceControls\": [],\n";
    const auto controlBoundary = source.find(controls, presentation);
    const std::string meters = "    \"performanceMeters\": []\n";
    const auto meterBoundary = source.find(meters, presentation);
    require(
        presentation != std::string::npos
            && version != std::string::npos
            && controlBoundary != std::string::npos
            && meterBoundary != std::string::npos,
        "fixture should contain empty presentation-v2 authority");
    source.erase(meterBoundary, meters.size());
    source.replace(
        controlBoundary,
        controls.size(),
        "    \"performanceControls\": []\n");
    source.replace(
        version,
        std::string{"    \"version\": 2,\n"}.size(),
        "    \"version\": 1,\n");
}

void eraseEmptySampleMapBoundary(std::string& source)
{
    const std::string boundary = R"json(  "sampleMapLibrary": {
    "version": 1,
    "maps": []
  },
)json";
    const auto position = source.find(boundary);
    require(position != std::string::npos, "fixture should contain empty sample-map boundary");
    source.erase(position, boundary.size());
}

void eraseEmptyPartialTrackEdits(std::string& source)
{
    const std::string boundary = "  \"partialTrackEdits\": [],\n";
    const auto position = source.find(boundary);
    require(position != std::string::npos,
            "fixture should contain empty partial-track edit boundary");
    source.erase(position, boundary.size());
}

void eraseEmptyLiveSetLibrary(std::string& source)
{
    const std::string boundary =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto position = source.find(boundary);
    require(position != std::string::npos,
        "fixture should contain empty live-set boundary");
    source.erase(position, boundary.size());
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
            "fixture should contain format-fifteen MIDI Clock authority");
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
}

void eraseCurrentExpressionLanguage(std::string& source)
{
    const std::string field =
        "  \"expressionLanguage\": \"variablesV2\",\n";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should contain current expression authority");
    source.erase(position, field.size());
    const std::string definitions = "  \"expressionDefinitions\": [],\n";
    const auto definitionsPosition = source.find(definitions);
    require(definitionsPosition != std::string::npos,
            "fixture should contain current expression definitions");
    source.erase(definitionsPosition, definitions.size());
    const std::string variables =
        "  \"variableLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto variablesPosition = source.find(variables);
    require(variablesPosition != std::string::npos,
            "fixture should contain current variable authority");
    source.erase(variablesPosition, variables.size());
    const std::string soundVariables =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto soundVariablesPosition = source.find(soundVariables);
    require(soundVariablesPosition != std::string::npos,
            "fixture should contain current Sound-variable authority");
    source.erase(soundVariablesPosition, soundVariables.size());
    const std::string generators =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    const auto generatorsPosition = source.find(generators);
    require(generatorsPosition != std::string::npos,
            "fixture should contain current generator authority");
    source.erase(generatorsPosition, generators.size());
}

talas::project::ProjectAsset audioAsset(
    std::string id,
    std::uint64_t frameCount)
{
    talas::project::ProjectAsset asset;
    asset.id = std::move(id);
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "samples/source.wav";
    asset.contentHash = std::string(64, 'a');
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = frameCount;
    asset.encoding = "pcm16-wav";
    return asset;
}

talas::project::ProjectSampleMap populatedMap()
{
    talas::project::ProjectSampleRegion region;
    region.regionId = "leadRegion";
    region.assetId = "leadSample";
    region.rootNote = 60;
    region.lowNote = 48;
    region.highNote = 72;
    region.lowVelocity = 17;
    region.highVelocity = 112;
    region.reverse = true;
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        10,
        90,
    };
    region.releaseLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        91,
        120,
    };

    talas::project::ProjectSampleMap map;
    map.mapId = "leadMap";
    map.displayName = "Lead Map";
    map.regions.push_back(std::move(region));
    return map;
}

void testPopulatedSampleMapRoundTripsCanonically()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 128));
    document.sampleMapLibrary.maps.push_back(populatedMap());

    require(
        talas::project::projectSampleMapDiagnostics(document).empty(),
        "valid sample-map state should have no diagnostics");

    const auto json = talas::project::serializeProjectDocument(document);
    requireContains(
        json,
        R"json("id": "leadMap")json",
        "sample-map identity should serialize");
    requireContains(
        json,
        R"json("endFrameExclusive": 90)json",
        "exact exclusive sustain-loop endpoint should serialize");
    requireContains(
        json,
        R"json("mode": "alternate")json",
        "alternate loop mode should serialize");

    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "canonical populated sample map should parse");
    require(
        loaded.document.sampleMapLibrary == document.sampleMapLibrary,
        "every sample-map field should round-trip exactly");
    require(
        talas::project::serializeProjectDocument(loaded.document) == json,
        "populated sample-map JSON should be byte-stable");
}

void testSampleMapDiagnosticsRejectBrokenIdentityAndRanges()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 64));
    auto map = populatedMap();
    map.regions.front().rootNote = 20;
    map.regions.front().sustainLoop->endFrameExclusive = 65;
    map.regions.push_back(map.regions.front());
    document.sampleMapLibrary.maps = {map, map};

    const auto diagnostics =
        talas::project::projectSampleMapDiagnostics(document);
    const auto hasPath = [&](std::string_view path) {
        return std::ranges::any_of(
            diagnostics,
            [&](const auto& diagnostic) {
                return diagnostic.path == path;
            });
    };
    require(
        hasPath("$.sampleMapLibrary.maps[0].regions[0].rootNote"),
        "root outside note range should be diagnosed precisely");
    require(
        hasPath("$.sampleMapLibrary.maps[0].regions[0].sustainLoop.endFrameExclusive"),
        "loop beyond the immutable asset shape should be diagnosed precisely");
    require(
        hasPath("$.sampleMapLibrary.maps[0].regions[1].id"),
        "duplicate region identity should be diagnosed precisely");
    require(
        hasPath("$.sampleMapLibrary.maps[1].id"),
        "duplicate map identity should be diagnosed precisely");
}

void testSampleMapDiagnosticsDoNotIndexInvalidNoteRanges()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 64));
    auto map = populatedMap();
    map.regions.front().rootNote = 200;
    map.regions.front().lowNote = 200;
    map.regions.front().highNote = 200;
    document.sampleMapLibrary.maps.push_back(std::move(map));

    const auto diagnostics =
        talas::project::projectSampleMapDiagnostics(document);
    const auto hasPath = [&](std::string_view path) {
        return std::ranges::any_of(
            diagnostics,
            [&](const auto& diagnostic) {
                return diagnostic.path == path;
            });
    };
    require(
        hasPath("$.sampleMapLibrary.maps[0].regions[0].lowNote")
            && hasPath("$.sampleMapLibrary.maps[0].regions[0].highNote")
            && hasPath("$.sampleMapLibrary.maps[0].regions[0].rootNote"),
        "invalid in-memory notes should be diagnosed without indexing the overlap table");
}

void testFormatElevenMigratesToEmptyFormatTwelveLibrary()
{
    auto versionEleven = talas::project::serializeProjectDocument({});
    const auto version = versionEleven.find(R"json("version": 156)json");
    require(version != std::string::npos, "fixture should contain current format version");
    versionEleven.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  11)json");
    eraseFormatSeventeenTransportSync(versionEleven);
    eraseFormatEighteenOscIngress(versionEleven);
    eraseFormatSixteenMusicalTime(versionEleven);
    eraseCurrentExpressionLanguage(versionEleven);
    downgradeEmptyPresentationToV1(versionEleven);
    eraseEmptySampleMapBoundary(versionEleven);
    eraseEmptyPartialTrackEdits(versionEleven);
    eraseEmptyLiveSetLibrary(versionEleven);

    const auto loaded = talas::project::parseProjectDocument(versionEleven);
    require(
        loaded.ok(),
        "format eleven should migrate without a sample-map collection: "
            + (loaded.errors.empty()
                    ? std::string{"unknown parse failure"}
                    : loaded.errors.front().path + " "
                        + loaded.errors.front().message));
    require(
        loaded.document.formatVersion == talas::project::currentProjectFormatVersion
            && loaded.document.sampleMapLibrary.schemaVersion == 1
            && loaded.document.sampleMapLibrary.maps.empty(),
        "format eleven should migrate to an empty format-twelve sample-map library");

    auto futureField = talas::project::serializeProjectDocument({});
    const auto futureVersion = futureField.find(R"json("version": 156)json");
    futureField.replace(
        futureVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  11)json");
    eraseFormatSeventeenTransportSync(futureField);
    eraseFormatEighteenOscIngress(futureField);
    eraseFormatSixteenMusicalTime(futureField);
    eraseCurrentExpressionLanguage(futureField);
    downgradeEmptyPresentationToV1(futureField);
    eraseEmptyPartialTrackEdits(futureField);
    requireLoadErrorAt(
        futureField,
        "$.sampleMapLibrary",
        "format eleven should reject future sample-map state");
}

void testFormatTwelveRequiresStrictSampleMapSchema()
{
    auto missingBoundary = talas::project::serializeProjectDocument({});
    eraseEmptySampleMapBoundary(missingBoundary);
    requireLoadErrorAt(
        missingBoundary,
        "$.sampleMapLibrary",
        "format twelve should require its versioned sample-map boundary");

    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 128));
    document.sampleMapLibrary.maps.push_back(populatedMap());
    auto unknownRegionField = talas::project::serializeProjectDocument(document);
    const auto reverse = unknownRegionField.find(R"json("reverse": true,)json");
    require(reverse != std::string::npos, "fixture should expose region reverse field");
    unknownRegionField.insert(
        reverse,
        R"json("future": 1,
            )json");
    requireLoadErrorAt(
        unknownRegionField,
        "$.sampleMapLibrary.maps[0].regions[0]",
        "format twelve should reject unknown sample-region fields");

    auto unsafeFrame = talas::project::serializeProjectDocument(document);
    const auto endFrame = unsafeFrame.find(R"json("endFrameExclusive": 90)json");
    require(endFrame != std::string::npos, "fixture should expose exact loop end frame");
    unsafeFrame.replace(
        endFrame,
        std::string_view{R"json("endFrameExclusive": 90)json"}.size(),
        R"json("endFrameExclusive": 9007199254740992)json");
    requireLoadErrorAt(
        unsafeFrame,
        "$.sampleMapLibrary.maps[0].regions[0].sustainLoop.endFrameExclusive",
        "sample loop frames should reject non-safe JSON integers");
}

void testProjectValidationIncludesSampleMapDiagnostics()
{
    talas::project::ProjectDocument document;
    document.assets.push_back(audioAsset("leadSample", 32));
    auto map = populatedMap();
    map.regions.front().sustainLoop->endFrameExclusive = 33;
    document.sampleMapLibrary.maps.push_back(std::move(map));

    const auto validation = talas::project::validateProjectDocument(
        document,
        talas::core::makeBuiltinNodeRegistry());
    require(
        std::ranges::any_of(
            validation.errors,
            [](const auto& error) {
                return error.code
                        == talas::core::ValidationCode::InvalidSampleMap
                    && error.nodeId
                        == "$.sampleMapLibrary.maps[0].regions[0].sustainLoop.endFrameExclusive";
            }),
        "project validation should retain precise sample-map diagnostics");
}

void testOriginalFormatTwelveFixtureIsCanonical()
{
    const auto path = std::filesystem::path{TALAS_SOURCE_DIR}
        / "tests"
        / "fixtures"
        / "project-format-13-sample-map.talas.json";
    std::ifstream input(path);
    require(input.good(), "original format-twelve sample-map fixture should be readable");
    std::string source{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
    if (source.ends_with('\n'))
        source.pop_back();

    const auto loaded = talas::project::parseProjectDocument(source);
    require(loaded.ok(), "original format-twelve sample-map fixture should parse");
    require(
        loaded.document.sampleMapLibrary.maps.size() == 1
            && loaded.document.sampleMapLibrary.maps.front().regions.size() == 2
            && talas::project::projectSampleMapDiagnostics(loaded.document).empty(),
        "original fixture should cover valid overlapping reverse and loop regions");
    const auto canonical =
        talas::project::serializeProjectDocument(loaded.document);
    require(
        canonical != source
            && canonical.find(R"json("version": 156)json")
                != std::string::npos,
        "retained format-thirteen fixture should migrate to canonical format fourteen");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "migrated sample-map fixture should be byte-stable after one canonical save");
}
}

int main()
{
    testPopulatedSampleMapRoundTripsCanonically();
    testSampleMapDiagnosticsRejectBrokenIdentityAndRanges();
    testSampleMapDiagnosticsDoNotIndexInvalidNoteRanges();
    testFormatElevenMigratesToEmptyFormatTwelveLibrary();
    testFormatTwelveRequiresStrictSampleMapSchema();
    testProjectValidationIncludesSampleMapDiagnostics();
    testOriginalFormatTwelveFixtureIsCanonical();
    return 0;
}
