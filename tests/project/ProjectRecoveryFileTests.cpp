#include "talas/project/ProjectRecoveryFile.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() / "talas-project-recovery-tests")
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error("Unable to create temporary recovery test directory");
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

talas::project::ProjectDocument documentWithTitle(std::string title)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {{"frequency", "220 hz"}},
    });
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "osc")->position = {48, 64};
    return document;
}

talas::project::ProjectDocument missingComponentDocument(
    std::string title)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    document.expressionDefinitions.definitions.push_back({
        "half",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value * 0.5",
    });
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    talas::project::ProjectParameterExpression mode;
    mode.parameterId = "mode";
    mode.choice = "colour";
    missing.parameters.push_back(std::move(mode));
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
    missing.preservedComponent = std::move(component);
    document.graph.nodes = {
        std::move(missing),
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"missing1", "audioOut", "out", "audioIn"},
    };
    document.virtualControls.push_back({
        "MacroA", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader, 0.0, -1.0, 1.0});
    document.virtualControls.push_back({
        "Gain", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader, 0.5, 0.0, 1.0});
    talas::project::reconcileProjectPresentation(document);
    document.presentation.performanceControls[0].kind =
        talas::project::ProjectPerformanceControlKind::Macro;
    document.presentation.performanceControls[0].modulationSlots.push_back({
        document.presentation.performanceControls[1].target, 0.75});
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {416, 96}, true});
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "missing1")
        ->position = {120, 80};
    return document;
}

talas::project::ProjectDocument documentWithSoundInterface(
    std::string title)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Voice";
    definition.interfaceVersion = 4;
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
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    document.graph.nodes.push_back(std::move(instance));
    return document;
}

talas::project::ProjectRecoveryRecord makeRecord(
    std::string cleanTitle,
    std::string recoveredTitle,
    std::optional<std::filesystem::path> projectPath,
    std::uint64_t recoveredAtMilliseconds = 1000)
{
    talas::project::ProjectRecoveryRecord record;
    record.projectPath = std::move(projectPath);
    record.cleanDocument =
        documentWithTitle(std::move(cleanTitle));
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle(std::move(recoveredTitle)),
            recoveredAtMilliseconds),
        "record fixture should append its recovered version");
    return record;
}

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string jsonStringLiteral(std::string_view value)
{
    std::string output{"\""};
    for (const auto character : value)
    {
        switch (character)
        {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default: output += character; break;
        }
    }
    output += '"';
    return output;
}

std::string versionTwoEnvelope(
    std::string_view cleanProject,
    std::string_view versionsJson)
{
    return
        "{\"format\":\"talas.project-recovery\",\"version\":2,"
        "\"projectPath\":null,\"cleanProject\":"
        + jsonStringLiteral(cleanProject)
        + ",\"versions\":["
        + std::string(versionsJson)
        + "]}";
}

std::string versionJson(
    std::uint64_t timestamp,
    std::string_view recoveredProject)
{
    return
        "{\"createdAtUnixMilliseconds\":"
        + std::to_string(timestamp)
        + ",\"recoveredProject\":"
        + jsonStringLiteral(recoveredProject)
        + "}";
}

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>{input},
        {});
}

void testRecordRoundTripsWithAndWithoutProjectIdentity()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    const auto projectPath = directory.path() / "My Sound.talas.json";

    auto identified =
        makeRecord(
            "Saved",
            "Recovered",
            projectPath);
    identified.projectRevision =
        talas::project::ProjectFileRevision{
            true,
            "{ exact raw project bytes }\n",
        };
    require(talas::project::saveProjectRecoveryFile(
                path,
                identified)
                .ok(),
            "recovery record should save");
    const auto loaded = talas::project::loadProjectRecoveryFile(path);
    require(loaded.ok(), "saved recovery record should load");
    require(!loaded.usedBackup, "normal recovery load should use the primary");
    require(loaded.record->projectPath
                == std::optional<std::filesystem::path>{projectPath},
            "project identity should round-trip");
    require(
        loaded.record->projectRevision
            == identified.projectRevision,
        "the exact pre-crash project-file revision should round-trip");
    require(loaded.record->cleanDocument.metadata.title == "Saved",
            "clean document should round-trip");
    require(loaded.record->versions.size() == 1
                && loaded.record->versions.back()
                       .recoveredDocument.metadata.title == "Recovered",
            "recovered document should round-trip");
    require(loaded.record->versions.back().createdAtUnixMilliseconds == 1000,
            "recovery timestamp should round-trip exactly");

    require(talas::project::saveProjectRecoveryFile(
                path,
                makeRecord("Untitled", "Untitled edits", {}))
                .ok(),
            "untitled recovery record should replace the prior record");
    const auto untitled = talas::project::loadProjectRecoveryFile(path);
    require(untitled.ok() && !untitled.record->projectPath.has_value(),
            "missing project identity should round-trip as untitled");
    require(
        !untitled.record->projectRevision.has_value(),
        "untitled recovery should not invent a disk revision");
}

void testRecoveryPreservesSoundInterfacesAndSparseOverrides()
{
    TemporaryDirectory directory;
    const auto path = directory.path()
        / "sound-interface.talas-recovery.json";
    talas::project::ProjectRecoveryRecord record;
    record.cleanDocument =
        documentWithSoundInterface("Clean interface");
    auto recovered =
        documentWithSoundInterface("Recovered interface");
    recovered.graph.nodes.front().parameters.push_back({
        "frequency",
        "220 hz",
    });
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            recovered,
            1234),
        "Sound interface recovery fixture should append");
    require(
        talas::project::saveProjectRecoveryFile(path, record).ok(),
        "Sound interface recovery record should save");

    const auto loaded = talas::project::loadProjectRecoveryFile(path);
    require(loaded.ok(),
            "Sound interface recovery record should load");
    const auto& cleanDefinition =
        loaded.record->cleanDocument.soundDefinitions.front();
    require(
        cleanDefinition.interfaceVersion == 4
            && cleanDefinition.exposedParameters.size() == 1
            && cleanDefinition.exposedParameters.front().aliases
                == std::vector<std::string>{"pitchHz"}
            && cleanDefinition.exposedParameters.front().bindings.size()
                == 1,
        "recovery should preserve the complete stable Sound interface descriptor");
    const auto& recoveredDocument =
        loaded.record->versions.back().recoveredDocument;
    require(
        recoveredDocument.graph.nodes.front().parameters.size() == 1
            && recoveredDocument.graph.nodes.front().parameters.front()
                   .parameterId
                == "frequency"
            && recoveredDocument.graph.nodes.front().parameters.front()
                   .expression
                == std::optional<std::string>{"220 hz"},
        "recovery should preserve sparse Sound instance overrides");
}

void testRecoverySourceRequiresExactRecordedRevision()
{
    const auto path =
        std::filesystem::path{
            "/sessions/source.talas.json"};
    auto record =
        makeRecord(
            "Saved",
            "Recovered",
            path);
    record.projectRevision =
        talas::project::ProjectFileRevision{
            true,
            "{\"saved\":true}\n",
        };
    const auto cleanSource =
        documentWithTitle("Saved");

    require(
        talas::project::
            projectRecoverySourceMatchesBaseline(
                record,
                cleanSource,
                *record.projectRevision),
        "recovery source should be reusable when semantic and exact raw baselines match");
    require(
        !talas::project::
             projectRecoverySourceMatchesBaseline(
                 record,
                 cleanSource,
                 talas::project::
                     ProjectFileRevision{
                         true,
                         "{ \"saved\": true }\n",
                     }),
        "formatting-only source changes should invalidate recovery identity");
    require(
        !talas::project::
             projectRecoverySourceMatchesBaseline(
                 record,
                 documentWithTitle(
                     "Externally changed"),
                 *record.projectRevision),
        "semantic source changes should invalidate recovery identity");

    record.projectRevision.reset();
    require(
        !talas::project::
             projectRecoverySourceMatchesBaseline(
                 record,
                 cleanSource,
                 talas::project::
                     ProjectFileRevision{
                         true,
                         "{\"saved\":true}\n",
                     }),
        "legacy recovery without a raw baseline should fail closed");
}

void testHistoryAppendIsMonotonicDeduplicatedAndBounded()
{
    talas::project::ProjectRecoveryRecord record;
    record.cleanDocument = documentWithTitle("Clean");
    for (std::uint64_t index = 0;
         index < talas::project::maximumProjectRecoveryVersions + 5;
         ++index)
    {
        require(
            talas::project::appendProjectRecoveryVersion(
                record,
                documentWithTitle(
                    "Version " + std::to_string(index)),
                500 + index),
            "distinct history fixture should append");
    }
    require(
        record.versions.size()
            == talas::project::maximumProjectRecoveryVersions,
        "history should retain only the bounded newest versions");
    require(
        record.versions.front().recoveredDocument.metadata.title
            == "Version 5"
            && record.versions.back().recoveredDocument.metadata.title
                == "Version 24",
        "retention should remove the oldest versions first");

    const auto priorTimestamp =
        record.versions.back().createdAtUnixMilliseconds;
    require(
        !talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle("Version 24"),
            1),
        "identical latest snapshot should report deduplication");
    require(
        record.versions.size()
            == talas::project::maximumProjectRecoveryVersions
            && record.versions.back().createdAtUnixMilliseconds
                == priorTimestamp,
        "identical consecutive snapshots should not be duplicated");

    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle("Clock did not advance"),
            1),
        "changed snapshot should append despite a stale clock");
    require(
        record.versions.back().createdAtUnixMilliseconds
            == priorTimestamp + 1,
        "new versions should remain strictly monotonic");

    talas::project::ProjectRecoveryRecord saturated;
    saturated.cleanDocument = documentWithTitle("Clean");
    require(
        talas::project::appendProjectRecoveryVersion(
            saturated,
            documentWithTitle("Future timestamp"),
            talas::project::maximumProjectRecoveryTimestamp),
        "maximum exact timestamp should be representable");
    require(
        talas::project::appendProjectRecoveryVersion(
            saturated,
            documentWithTitle("Current edit"),
            1234),
        "a changed snapshot should not be mistaken for deduplication after timestamp saturation");
    require(
        saturated.versions.size() == 1
            && saturated.versions.front()
                   .createdAtUnixMilliseconds == 1234
            && saturated.versions.front()
                   .recoveredDocument.metadata.title
                == "Current edit",
        "timestamp saturation should begin a safe new history epoch");
}

void testCalendarDisplayRangeRejectsExtremeValidTimestamps()
{
    require(
        talas::project::projectRecoveryTimestampIsCalendarDisplaySafe(1000),
        "ordinary post-epoch timestamps should be safe for calendar display");
    require(
        talas::project::projectRecoveryTimestampIsCalendarDisplaySafe(
            talas::project::maximumProjectRecoveryCalendarDisplayTimestamp),
        "the documented calendar display boundary should be inclusive");
    require(
        !talas::project::projectRecoveryTimestampIsCalendarDisplaySafe(
            talas::project::maximumProjectRecoveryCalendarDisplayTimestamp + 1),
        "timestamps beyond the portable calendar range should use a numeric fallback");
    require(
        !talas::project::projectRecoveryTimestampIsCalendarDisplaySafe(
            talas::project::maximumProjectRecoveryTimestamp),
        "the JSON-safe maximum must not reach platform calendar conversion");
}

void testVersionThreeRoundTripsMultipleVersionsAndLegacyVersionsMigrate()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "current.talas-recovery.json";
    auto record = makeRecord("Saved", "First", {}, 100);
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle("Second"),
            200),
        "second version should append");
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle("Third"),
            300),
        "third version should append");
    require(
        talas::project::saveProjectRecoveryFile(path, record).ok(),
        "multi-version archive should save");
    const auto loaded =
        talas::project::loadProjectRecoveryFile(path);
    require(
        loaded.ok() && loaded.record->versions.size() == 3,
        "multi-version archive should round-trip");
    require(
        loaded.record->versions[0].recoveredDocument.metadata.title
                == "First"
            && loaded.record->versions[2]
                   .recoveredDocument.metadata.title == "Third",
        "version order should remain oldest to newest");
    require(
        talas::project::projectRecoveryVersionIndexNewestFirst(
            *loaded.record,
            0)
                == std::optional<std::size_t>{2}
            && talas::project::projectRecoveryVersionIndexNewestFirst(
                   *loaded.record,
                   2)
                == std::optional<std::size_t>{0}
            && !talas::project::projectRecoveryVersionIndexNewestFirst(
                    *loaded.record,
                    3)
                    .has_value(),
        "newest-first browser positions should map to exact archive versions");

    const auto clean =
        talas::project::serializeProjectDocument(
            documentWithTitle("Legacy clean"));
    const auto recovered =
        talas::project::serializeProjectDocument(
            documentWithTitle("Legacy recovered"));
    writeText(
        path,
        versionTwoEnvelope(
            clean,
            versionJson(
                100,
                recovered)));
    const auto migratedHistory =
        talas::project::loadProjectRecoveryFile(
            path);
    require(
        migratedHistory.ok()
            && migratedHistory.record->versions
                    .size()
                == 1
            && !migratedHistory.record
                    ->projectRevision
                    .has_value(),
        "version-two history should load without inventing a raw disk baseline");

    std::string legacy =
        "{\"format\":\"talas.project-recovery\",\"version\":1,"
        "\"projectPath\":null,\"cleanProject\":"
        + jsonStringLiteral(clean)
        + ",\"recoveredProject\":"
        + jsonStringLiteral(recovered)
        + "}";
    writeText(path, legacy);
    const auto migrated =
        talas::project::loadProjectRecoveryFile(path);
    require(
        migrated.ok()
            && migrated.record->versions.size() == 1
            && migrated.record->versions.front()
                   .createdAtUnixMilliseconds == 0
            && migrated.record->versions.front()
                   .recoveredDocument.metadata.title
                == "Legacy recovered",
        "version-one archive should load as one legacy version");
    require(
        talas::project::saveProjectRecoveryFile(
            path,
            *migrated.record)
            .ok()
            && readText(path).find("\"version\": 3")
                != std::string::npos,
        "saving a migrated archive should publish version three");
}

void testInvalidVersionArraysAndSaveRecordsAreRejected()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "current.talas-recovery.json";
    const auto clean =
        talas::project::serializeProjectDocument(
            documentWithTitle("Clean"));
    const auto recovered =
        talas::project::serializeProjectDocument(
            documentWithTitle("Recovered"));

    writeText(path, versionTwoEnvelope(clean, ""));
    require(
        !talas::project::loadProjectRecoveryFile(path).ok(),
        "empty recovery history should be rejected");

    writeText(
        path,
        versionTwoEnvelope(
            clean,
            versionJson(2, recovered)
                + ","
                + versionJson(1, recovered)));
    require(
        !talas::project::loadProjectRecoveryFile(path).ok(),
        "unordered recovery timestamps should be rejected");

    writeText(
        path,
        versionTwoEnvelope(
            clean,
            versionJson(1, "{}")));
    require(
        !talas::project::loadProjectRecoveryFile(path).ok(),
        "invalid nested recovery projects should be rejected");

    std::string oversizedVersions;
    for (std::size_t index = 0;
         index < talas::project::maximumProjectRecoveryVersions + 1;
         ++index)
    {
        if (!oversizedVersions.empty())
            oversizedVersions += ",";
        oversizedVersions += versionJson(
            static_cast<std::uint64_t>(index + 1),
            recovered);
    }
    writeText(
        path,
        versionTwoEnvelope(clean, oversizedVersions));
    require(
        !talas::project::loadProjectRecoveryFile(path).ok(),
        "oversized recovery history should be rejected");

    talas::project::ProjectRecoveryRecord emptyRecord;
    emptyRecord.cleanDocument =
        documentWithTitle("Clean");
    const auto rejected =
        talas::project::saveProjectRecoveryFile(
            directory.path() / "empty.talas-recovery.json",
            emptyRecord);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectRecoveryErrorCode::WriteFailed,
        "saving an empty archive should fail before publication");
}

void testMissingMalformedAndInvalidNestedProjectsAreStructuralFailures()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";

    const auto missing = talas::project::loadProjectRecoveryFile(path);
    require(missing.missing(), "missing recovery should be distinguishable from corruption");
    require(missing.error->code == talas::project::ProjectRecoveryErrorCode::NotFound,
            "missing recovery should report NotFound");

    writeText(path, "{ invalid");
    const auto malformed = talas::project::loadProjectRecoveryFile(path);
    require(!malformed.ok() && !malformed.missing(),
            "malformed recovery envelope should fail");
    require(malformed.error->code
                == talas::project::ProjectRecoveryErrorCode::InvalidRecovery,
            "malformed recovery should report InvalidRecovery");

    writeText(
        path,
        R"json({"format":"talas.project-recovery","version":1,"projectPath":null,"cleanProject":"{}","recoveredProject":"{}"})json");
    const auto invalidProjects = talas::project::loadProjectRecoveryFile(path);
    require(!invalidProjects.ok(),
            "recovery containing invalid project documents should fail");
    require(invalidProjects.error->code
                == talas::project::ProjectRecoveryErrorCode::InvalidRecovery,
            "invalid nested project should report InvalidRecovery");
}

void testCancellationPreservesPriorRecord()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    require(talas::project::saveProjectRecoveryFile(
                path,
                makeRecord("Saved", "Prior recovery", {}))
                .ok(),
            "cancellation fixture should save its prior recovery");

    int cancellationChecks = 0;
    const auto cancelled = talas::project::saveProjectRecoveryFile(
        path,
        makeRecord("Saved", "Must not publish", {}),
        [&cancellationChecks] {
            ++cancellationChecks;
            return cancellationChecks >= 2;
        });
    require(cancelled.cancelled(), "cancelled save should report cancellation");
    require(!std::filesystem::exists(path.string() + ".tmp"),
            "cancelled save should remove its temporary file");

    const auto loaded = talas::project::loadProjectRecoveryFile(path);
    require(loaded.ok()
                && loaded.record->versions.back()
                       .recoveredDocument.metadata.title
                    == "Prior recovery",
            "cancelled save must preserve the prior recovery record");
}

void testInvalidSampleMapsCannotPublishRecoveryDocuments()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    require(
        talas::project::saveProjectRecoveryFile(
            path,
            makeRecord("Saved", "Prior recovery", {})).ok(),
        "invalid sample-map recovery fixture should save a valid baseline");
    const auto baseline = readText(path);

    auto invalidClean = makeRecord(
        "Invalid clean",
        "Recovered",
        {});
    invalidClean.cleanDocument.sampleMapLibrary.schemaVersion = 2;
    const auto rejectedClean =
        talas::project::saveProjectRecoveryFile(
            path,
            invalidClean);
    require(
        !rejectedClean.ok()
            && rejectedClean.error->code
                == talas::project::ProjectRecoveryErrorCode::WriteFailed
            && readText(path) == baseline,
        "invalid clean sample-map state should preserve the prior recovery bytes");

    auto invalidRecovered = makeRecord(
        "Saved",
        "Invalid recovered",
        {});
    invalidRecovered.versions.front()
        .recoveredDocument.sampleMapLibrary.schemaVersion = 2;
    const auto rejectedRecovered =
        talas::project::saveProjectRecoveryFile(
            path,
            invalidRecovered);
    require(
        !rejectedRecovered.ok()
            && rejectedRecovered.error->code
                == talas::project::ProjectRecoveryErrorCode::WriteFailed
            && readText(path) == baseline,
        "invalid recovered sample-map state should preserve the prior recovery bytes");

    const auto missingParent = directory.path()
        / "missing" / "nested.talas-recovery.json";
    require(
        !talas::project::saveProjectRecoveryFile(
             missingParent,
             invalidRecovered).ok()
            && !std::filesystem::exists(
                missingParent.parent_path()),
        "invalid recovery document should fail before parent creation");
}

void testNonRegularDestinationIsRejected()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    std::filesystem::create_directory(path);

    const auto saved = talas::project::saveProjectRecoveryFile(
        path,
        makeRecord("Saved", "Recovered", {}));
    require(!saved.ok(), "directory recovery destination should be rejected");
    require(saved.error->code == talas::project::ProjectRecoveryErrorCode::WriteFailed,
            "non-regular destination should report WriteFailed");
    require(std::filesystem::is_directory(path),
            "rejected destination directory should remain intact");

    const auto target = directory.path() / "unrelated.txt";
    writeText(target, "unrelated");
    const auto link = directory.path() / "linked.talas-recovery.json";
    std::filesystem::create_symlink(target, link);
    const auto linkedSave = talas::project::saveProjectRecoveryFile(
        link,
        makeRecord("Saved", "Recovered", {}));
    require(!linkedSave.ok(),
            "symlink recovery destination should be rejected");
    std::ifstream targetInput(target);
    const std::string targetText(
        std::istreambuf_iterator<char>{targetInput},
        {});
    require(targetText == "unrelated",
            "rejected symlink destination must not alter its target");
}

void testTemporarySymlinkNeverTruncatesItsTarget()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    auto temporary = path;
    temporary += ".tmp";
    const auto unrelated = directory.path() / "unrelated-recovery.txt";
    writeText(unrelated, "must remain unchanged");
    std::filesystem::create_symlink(unrelated, temporary);

    require(
        talas::project::saveProjectRecoveryFile(
            path,
            makeRecord("Saved", "Recovered", {}))
            .ok(),
        "removable recovery temporary symlink should be replaced safely");
    std::ifstream input(unrelated, std::ios::binary);
    const std::string text(
        std::istreambuf_iterator<char>{input},
        {});
    require(text == "must remain unchanged",
            "recovery temporary symlink target must never be truncated");
    require(!std::filesystem::is_symlink(temporary),
            "recovery temporary symlink should not survive successful save");
}

void testBackupLoadsWhenPrimaryIsAbsent()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    const auto backup = std::filesystem::path(path.string() + ".bak");
    require(talas::project::saveProjectRecoveryFile(
                path,
                makeRecord("Saved", "Backup recovery", {}))
                .ok(),
            "backup fixture should save");
    std::filesystem::rename(path, backup);

    const auto loaded = talas::project::loadProjectRecoveryFile(path);
    require(loaded.ok() && loaded.usedBackup,
            "loader should recover the fallback backup when primary is absent");
    require(loaded.record->versions.back()
                .recoveredDocument.metadata.title
            == "Backup recovery",
            "fallback backup should preserve recovered content");

    require(talas::project::saveProjectRecoveryFile(
                path,
                makeRecord("Saved", "New recovery", {}))
                .ok(),
            "a later save should repair a backup-only recovery state");
    require(std::filesystem::exists(path) && !std::filesystem::exists(backup),
            "successful save should publish primary recovery and remove fallback backup");
}

void testDiscardRemovesPrimaryAndSidecars()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "current.talas-recovery.json";
    writeText(path, "primary");
    writeText(path.string() + ".tmp", "temporary");
    writeText(path.string() + ".bak", "backup");

    const auto discarded = talas::project::discardProjectRecoveryFile(path);
    require(discarded.ok(), "discard should remove recovery artifacts");
    require(!std::filesystem::exists(path), "discard should remove primary recovery");
    require(!std::filesystem::exists(path.string() + ".tmp"),
            "discard should remove recovery temporary");
    require(!std::filesystem::exists(path.string() + ".bak"),
            "discard should remove recovery backup");
    require(talas::project::discardProjectRecoveryFile(path).ok(),
            "discard should be idempotent when recovery is already absent");
}

void testUnsafeSourceIdentityKeepsRelativeAssetsResolvableAcrossSaveAs()
{
    auto record = makeRecord(
        "Saved",
        "Recovered",
        std::filesystem::path("/old/session/sound.talas.json"));
    record.cleanDocument.assets.push_back({
        "Samples/Hit",
        talas::project::ProjectAssetKind::Table,
        "assets/hit.wav",
    });
    record.cleanDocument.assets.front().contentHash.assign(64, 'a');
    record.cleanDocument.assets.front().generation = 5;
    record.cleanDocument.assets.front().provenance = "legacy import";
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            documentWithTitle("Later recovered"),
            2000),
        "asset history fixture should append a second version");
    for (auto& version : record.versions)
    {
        version.recoveredDocument.assets =
            record.cleanDocument.assets;
    }

    require(
        talas::project::makeProjectRecoveryAssetPathsAbsolute(
            record,
            *record.projectPath),
        "unsafe recovery should detach clean and every version from the old source");
    const auto expected =
        std::filesystem::path("/old/session/assets/hit.wav");
    require(
        talas::project::pathFromUtf8(record.cleanDocument.assets.front().path)
                .lexically_normal()
            == expected,
        "clean recovery asset should become absolute against the old source");
    require(
        record.cleanDocument.assets.front().contentHash
                == std::string(64, 'a')
            && record.cleanDocument.assets.front().generation == 5
            && record.cleanDocument.assets.front().provenance
                == "legacy import",
        "recovery detachment should change only asset paths, not accepted manifest identity");
    require(
        talas::project::pathFromUtf8(
            record.versions.back()
                .recoveredDocument.assets.front().path)
                .lexically_normal()
            == expected,
        "recovered asset should remain resolvable after source identity is dropped");
    for (const auto& version : record.versions)
    {
        require(
            talas::project::pathFromUtf8(
                version.recoveredDocument
                    .assets.front().path)
                == expected,
            "every recovery version should retain the detached asset target");
    }

    require(talas::project::rebaseProjectAssetPaths(
                record.versions.back().recoveredDocument,
                {},
                "/new/session/recovered.talas.json"),
            "later Save As should rebase the absolute recovered asset");
    require(
        talas::project::resolveProjectAssetPath(
            record.versions.back()
                .recoveredDocument.assets.front(),
            std::filesystem::path("/new/session/recovered.talas.json"))
            == expected,
        "Save As should preserve the recovered asset's original absolute target");
}

void testVersionOnlyAssetDetachmentReportsAConversion()
{
    auto record = makeRecord(
        "Saved",
        "Recovered",
        std::filesystem::path("/old/session/sound.talas.json"));
    record.versions.back().recoveredDocument.assets.push_back({
        "Version-only sample",
        talas::project::ProjectAssetKind::Table,
        "assets/version-only.wav",
    });

    require(
        talas::project::makeProjectRecoveryAssetPathsAbsolute(
            record,
            *record.projectPath),
        "detachment should report a conversion when only a retained version changes");
    require(
        talas::project::pathFromUtf8(
            record.versions.back()
                .recoveredDocument.assets.front().path)
            == std::filesystem::path(
                "/old/session/assets/version-only.wav"),
        "the version-only relative asset should become absolute");
}

void testSuccessfulPublicationIsRetainedAcrossFailedReplacement()
{
    auto retained = makeRecord("Saved", "First recovery", {}).versions;
    auto publication = retained;
    publication.push_back({
        2000,
        documentWithTitle("Published before replacement"),
    });
    const talas::project::ProjectRecoverySaveResult successfulWrite;

    require(
        talas::project::consumeSuccessfulProjectRecoveryWrite(
            successfulWrite,
            publication,
            retained),
        "a successful worker publication should be consumed before replacement");

    const auto replacementSucceeded = false;
    if (!replacementSucceeded)
    {
        talas::project::ProjectRecoveryRecord nextRecord;
        nextRecord.versions = retained;
        require(
            talas::project::appendProjectRecoveryVersion(
                nextRecord,
                documentWithTitle("Edit after failed replacement"),
                3000),
            "the unchanged document generation should remain autosaveable");
        require(
            nextRecord.versions.size() == 3,
            "the next autosave should retain the worker-published version");
        require(
            nextRecord.versions[1].recoveredDocument.metadata.title
                == "Published before replacement",
            "failed replacement must not roll history back behind the published archive");
    }
}

void testUnicodeAssetPathsUseUtf8AcrossResolutionAndRebase()
{
    TemporaryDirectory directory;
    const auto unicodeFile =
        std::filesystem::path(std::u8string{u8"κ uzorak.wav"});
    const auto storedRelative = std::filesystem::path("assets") / unicodeFile;
    const auto sourceProject =
        directory.path() / std::filesystem::path(std::u8string{u8"Projekt κ.talas.json"});
    const auto expected =
        (directory.path() / storedRelative).lexically_normal();

    talas::project::ProjectDocument document;
    document.assets.push_back({
        "Unicode",
        talas::project::ProjectAssetKind::Table,
        talas::project::pathToUtf8(storedRelative),
    });

    require(
        talas::project::resolveProjectAssetPath(
            document.assets.front(),
            sourceProject)
            == expected,
        "UTF-8 relative asset path should resolve through the native filesystem");
    require(
        talas::project::makeProjectAssetPathsAbsolute(document, sourceProject),
        "UTF-8 relative asset path should become absolute");
    require(
        talas::project::pathFromUtf8(document.assets.front().path) == expected,
        "absolute asset path should remain UTF-8 encoded in project state");

    const auto targetProject =
        directory.path() / "moved" / "sound.talas.json";
    require(
        talas::project::rebaseProjectAssetPaths(document, {}, targetProject),
        "UTF-8 absolute asset path should rebase for Save As");
    require(
        talas::project::resolveProjectAssetPath(
            document.assets.front(),
            targetProject)
            == expected,
        "rebased UTF-8 asset path should preserve the original native target");
}

void testRecoveryPreservesMissingComponentCapsulesExactly()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "missing.talas-recovery.json";
    talas::project::ProjectRecoveryRecord record;
    record.cleanDocument =
        missingComponentDocument("Clean missing");
    require(
        talas::project::appendProjectRecoveryVersion(
            record,
            missingComponentDocument("Recovered missing"),
            1000),
        "missing-component recovery fixture should append one version");
    const auto cleanBefore =
        talas::project::serializeProjectDocument(
            record.cleanDocument);
    const auto recoveredBefore =
        talas::project::serializeProjectDocument(
            record.versions.front().recoveredDocument);

    require(
        talas::project::saveProjectRecoveryFile(path, record).ok(),
        "missing-component recovery record should save");
    const auto loaded =
        talas::project::loadProjectRecoveryFile(path);
    require(
        loaded.ok()
            && loaded.record->versions.size() == 1
            && talas::project::serializeProjectDocument(
                   loaded.record->cleanDocument)
                == cleanBefore
            && talas::project::serializeProjectDocument(
                   loaded.record->versions.front()
                       .recoveredDocument)
                == recoveredBefore,
        "recovery should retain capsule, expression definitions, opaque state, parameters, topology, and presentation byte-exact");
}
}

int main()
{
    testRecordRoundTripsWithAndWithoutProjectIdentity();
    testRecoveryPreservesSoundInterfacesAndSparseOverrides();
    testRecoverySourceRequiresExactRecordedRevision();
    testHistoryAppendIsMonotonicDeduplicatedAndBounded();
    testCalendarDisplayRangeRejectsExtremeValidTimestamps();
    testVersionThreeRoundTripsMultipleVersionsAndLegacyVersionsMigrate();
    testInvalidVersionArraysAndSaveRecordsAreRejected();
    testMissingMalformedAndInvalidNestedProjectsAreStructuralFailures();
    testCancellationPreservesPriorRecord();
    testInvalidSampleMapsCannotPublishRecoveryDocuments();
    testNonRegularDestinationIsRejected();
    testTemporarySymlinkNeverTruncatesItsTarget();
    testBackupLoadsWhenPrimaryIsAbsent();
    testDiscardRemovesPrimaryAndSidecars();
    testUnsafeSourceIdentityKeepsRelativeAssetsResolvableAcrossSaveAs();
    testVersionOnlyAssetDetachmentReportsAConversion();
    testSuccessfulPublicationIsRetainedAcrossFailedReplacement();
    testUnicodeAssetPathsUseUtf8AcrossResolutionAndRebase();
    testRecoveryPreservesMissingComponentCapsulesExactly();
    return 0;
}
