#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() / "talas-project-file-tests")
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        if (error)
            throw std::runtime_error("Unable to create temporary test directory");
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

talas::project::ProjectDocument makeDocument(std::string title, std::int32_t x)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    document.graph.nodes.push_back(
        {"osc1", "talas.oscillator", {{"frequency", "220 hz"}}});
    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "osc1")->position = {x, -48};
    return document;
}

talas::project::ProjectDocument makeMissingComponentDocument()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Preserved missing component";
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        {},
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    missing.preservedComponent = std::move(component);
    document.graph.nodes.push_back(std::move(missing));
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
        document.presentation.performanceControls[1].target, 0.5});
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {416, 96}, true});
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "missing1")
        ->position = {120, 80};
    return document;
}

talas::project::ProjectDocument makeSampleMapDocument()
{
    auto document = makeDocument("Sample map", 0);
    talas::project::ProjectAsset asset;
    asset.id = "audio_main";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "audio.wav";
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = 128;
    asset.encoding = "pcm16-wav";
    document.assets.push_back(std::move(asset));
    talas::project::ProjectSampleRegion region{
        "region_main",
        "audio_main",
        60,
        60,
        60,
        1,
        127,
    };
    region.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Forward,
        16,
        96,
    };
    document.sampleMapLibrary.maps.push_back({
        "map_main",
        "Main",
        {std::move(region)},
    });
    return document;
}

std::string readText(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

void testSaveLoadAndReplaceProjectFile()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "sound.talas.json";

    const auto initialSave = talas::project::saveProjectFile(path, makeDocument("Initial", 320));
    require(initialSave.ok(), "initial project save should succeed");
    require(std::filesystem::exists(path), "saved project should exist");

    const auto loaded = talas::project::loadProjectFile(path);
    require(loaded.ok(), "saved project should load");
    require(
        initialSave.revision.has_value()
            && loaded.revision
                == initialSave.revision,
        "successful save and load should expose the exact same disk revision");
    require(loaded.document->metadata.title == "Initial", "saved title should round-trip");
    require(talas::project::findProjectNodePresentation(
                *loaded.document,
                std::nullopt,
                "osc1")->position.x == 320,
            "saved canvas position should round-trip");

    const auto replacementSave = talas::project::saveProjectFile(path, makeDocument("Replacement", -32));
    require(replacementSave.ok(), "saving over an existing project should succeed");
    const auto replacement = talas::project::loadProjectFile(path);
    require(replacement.ok(), "replacement project should load");
    require(replacement.document->metadata.title == "Replacement", "replacement should overwrite old content");
    require(talas::project::findProjectNodePresentation(
                *replacement.document,
                std::nullopt,
                "osc1")->position.x == -32,
            "replacement position should overwrite old content");
    require(!std::filesystem::exists(path.string() + ".tmp"), "successful save should remove temporary file");
    require(!std::filesystem::exists(path.string() + ".bak"), "successful save should remove backup file");

    // An open Sound file is a copy in memory, edited
    // there, the file on the disk unchanged until it is saved back.
    const auto bytesOnDisk = [&path] {
        std::ifstream input(path, std::ios::binary);
        return std::string{std::istreambuf_iterator<char>{input}, {}};
    };
    const auto before = bytesOnDisk();
    auto edited = *replacement.document;
    edited.metadata.title = "Edited";
    require(bytesOnDisk() == before, "editing the copy in memory should leave the file as it was");
    require(talas::project::saveProjectFile(path, edited).ok() && talas::project::loadProjectFile(path).document->metadata.title == "Edited",
        "saving should put the edited copy back on the disk");

    // A Sound's sample file is saved by its name,
    // not its contents.
    const auto samplePath = directory.path() / "sampled.talas.json";
    require(talas::project::saveProjectFile(samplePath, makeSampleMapDocument()).ok(), "the sampled project should save");
    std::ifstream sampled(samplePath, std::ios::binary);
    const std::string sampledText{std::istreambuf_iterator<char>{sampled}, {}};
    require(sampledText.find("\"audio.wav\"") != std::string::npos && sampledText.find("RIFF") == std::string::npos,
        "only the sample file's name should be saved with the Sound");
}

void testCheckedSaveRejectsExternalContentAndFormattingChanges()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "checked.talas.json";
    require(
        talas::project::saveProjectFile(
            path,
            makeDocument("Initial", 0))
            .ok(),
        "checked-save fixture should save");
    const auto loaded =
        talas::project::loadProjectFile(path);
    require(
        loaded.ok()
            && loaded.revision.has_value(),
        "checked-save fixture should retain a revision");

    const auto unchanged =
        talas::project::saveProjectFile(
            path,
            makeDocument("Local", 1),
            loaded.revision);
    require(
        unchanged.ok()
            && unchanged.revision.has_value(),
        "checked save should replace an unchanged source");

    const auto externalText =
        readText(path) + "\n ";
    {
        std::ofstream external(
            path,
            std::ios::binary | std::ios::trunc);
        external << externalText;
    }
    const auto conflicted =
        talas::project::saveProjectFile(
            path,
            makeDocument("Must not win", 2),
            unchanged.revision);
    require(
        !conflicted.ok()
            && conflicted.error->code
                == talas::project::
                    ProjectFileErrorCode::
                        ExternalConflict
            && readText(path) == externalText,
        "formatting-only external changes should reject checked save without mutation");
}

void testCheckedSaveRejectsDeletionAndCreateAfterMissing()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "deleted.talas.json";
    const auto initial =
        talas::project::saveProjectFile(
            path,
            makeDocument("Initial", 0));
    require(
        initial.ok()
            && initial.revision.has_value(),
        "deletion fixture should save");
    std::filesystem::remove(path);
    const auto deletedConflict =
        talas::project::saveProjectFile(
            path,
            makeDocument("Local", 1),
            initial.revision);
    require(
        !deletedConflict.ok()
            && deletedConflict.error->code
                == talas::project::
                    ProjectFileErrorCode::
                        ExternalConflict
            && !std::filesystem::exists(path),
        "external deletion should reject checked save and remain deleted");

    const auto missingRevision =
        talas::project::captureProjectFileRevision(
            path);
    require(
        missingRevision.ok()
            && missingRevision.revision.has_value()
            && !missingRevision.revision->exists,
        "missing destination should have a comparable revision");
    const auto externalText =
        std::string{"external creation"};
    {
        std::ofstream external(
            path,
            std::ios::binary);
        external << externalText;
    }
    const auto createdConflict =
        talas::project::saveProjectFile(
            path,
            makeDocument("Must not win", 2),
            missingRevision.revision);
    require(
        !createdConflict.ok()
            && createdConflict.error->code
                == talas::project::
                    ProjectFileErrorCode::
                        ExternalConflict
            && readText(path) == externalText,
        "create-after-missing should reject checked save without mutation");
}

void testSaveRejectsDirectoryDestination()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "directory.talas.json";
    std::filesystem::create_directories(path);
    {
        std::ofstream marker(path / "keep.txt");
        marker << "keep";
    }
    const auto saved =
        talas::project::saveProjectFile(
            path,
            makeDocument("Unsafe", 0));
    require(
        !saved.ok()
            && readText(path / "keep.txt") == "keep",
        "project save must reject a directory destination without mutation");
}

void testLoadReportsIoAndParseFailures()
{
    TemporaryDirectory directory;
    const auto missing = talas::project::loadProjectFile(directory.path() / "missing.talas.json");
    require(!missing.ok(), "missing project should fail to load");
    require(!missing.errors.empty(), "missing project should report an error");
    require(missing.errors.front().code == talas::project::ProjectFileErrorCode::ReadFailed,
            "missing project should report read failure");

    const auto malformedPath = directory.path() / "malformed.talas.json";
    std::ofstream malformed(malformedPath);
    malformed << "{ invalid json";
    malformed.close();

    const auto malformedLoad = talas::project::loadProjectFile(malformedPath);
    require(!malformedLoad.ok(), "malformed project should fail to load");
    require(!malformedLoad.errors.empty(), "malformed project should report an error");
    require(malformedLoad.errors.front().code == talas::project::ProjectFileErrorCode::InvalidProject,
            "malformed project should report invalid project");
}

void testUnownedTemporaryArtifactsArePreserved()
{
    TemporaryDirectory directory;
    const auto unrelated = directory.path() / "unrelated.txt";
    {
        std::ofstream output(unrelated, std::ios::binary);
        output << "must remain unchanged";
    }
    const auto symlinkProject =
        directory.path() / "symlink.talas.json";
    auto symlinkTemporary = symlinkProject;
    symlinkTemporary += ".tmp";
    std::filesystem::create_symlink(
        unrelated,
        symlinkTemporary);

    require(
        talas::project::saveProjectFile(
            symlinkProject,
            makeDocument("Safe", 0))
            .ok(),
        "unowned temporary symlink should not block safe save");
    std::ifstream input(unrelated, std::ios::binary);
    const std::string text(
        std::istreambuf_iterator<char>{input},
        {});
    require(text == "must remain unchanged",
            "project temporary symlink target must never be truncated");
    require(
        std::filesystem::is_symlink(
            symlinkTemporary),
        "unowned project temporary symlink should be preserved");

    const auto fileProject =
        directory.path() / "file.talas.json";
    auto fileTemporary = fileProject;
    fileTemporary += ".tmp";
    {
        std::ofstream temporary(fileTemporary);
        temporary << "preserve file";
    }
    require(
        talas::project::saveProjectFile(
            fileProject,
            makeDocument("Safe", 1))
            .ok()
            && readText(fileTemporary)
                == "preserve file",
        "unowned temporary regular file should be preserved");

    const auto directoryProject =
        directory.path() / "tree.talas.json";
    auto directoryTemporary = directoryProject;
    directoryTemporary += ".tmp";
    std::filesystem::create_directories(
        directoryTemporary);
    {
        std::ofstream marker(
            directoryTemporary / "keep.txt");
        marker << "preserve tree";
    }
    require(
        talas::project::saveProjectFile(
            directoryProject,
            makeDocument("Safe", 2))
            .ok()
            && readText(
                directoryTemporary
                / "keep.txt")
                == "preserve tree",
        "unowned temporary directory should be preserved");
}

void testUnownedBackupSymlinkIsPreserved()
{
    TemporaryDirectory directory;
    const auto path =
        directory.path() / "backup.talas.json";
    require(
        talas::project::saveProjectFile(
            path,
            makeDocument("Initial", 0))
            .ok(),
        "backup-symlink fixture should save");

    auto backup = path;
    backup += ".bak";
    const auto missingTarget =
        directory.path() / "missing-target";
    std::filesystem::create_symlink(
        missingTarget,
        backup);

    const auto saved =
        talas::project::saveProjectFile(
            path,
            makeDocument("Replacement", 1));
#if defined(_WIN32)
    require(
        !saved.ok()
            && std::filesystem::is_symlink(
                backup)
            && readText(path)
                .find("Initial")
                != std::string::npos,
        "an unowned dangling backup symlink should reject save without mutating either artifact");
#else
    require(
        saved.ok()
            && std::filesystem::is_symlink(backup)
            && readText(path).find("Replacement") != std::string::npos,
        "an unowned backup symlink must neither block an atomic save nor be touched by it");
#endif
}

void testBackupOnlyStatesDoNotBlockSave()
{
#if defined(_WIN32)

    TemporaryDirectory directory;

    const auto filePath =
        directory.path() / "file-backup.talas.json";
    auto fileBackup = filePath;
    fileBackup += ".bak";
    {
        std::ofstream output(
            fileBackup,
            std::ios::binary);
        output << "preserved original";
    }
    require(
        !talas::project::saveProjectFile(
             filePath,
             makeDocument("Must not publish", 0))
             .ok()
            && !std::filesystem::exists(
                filePath)
            && readText(fileBackup)
                == "preserved original",
        "a regular backup without a primary should reject save and remain recoverable");

    const auto directoryPath =
        directory.path()
        / "directory-backup.talas.json";
    auto directoryBackup = directoryPath;
    directoryBackup += ".bak";
    std::filesystem::create_directories(
        directoryBackup);
    {
        std::ofstream marker(
            directoryBackup / "keep.txt");
        marker << "preserve directory";
    }
    require(
        !talas::project::saveProjectFile(
             directoryPath,
             makeDocument("Must not publish", 1))
             .ok()
            && !std::filesystem::exists(
                directoryPath)
            && readText(
                directoryBackup / "keep.txt")
                == "preserve directory",
        "a backup directory without a primary should reject save without mutation");

    const auto symlinkPath =
        directory.path()
        / "symlink-backup.talas.json";
    auto symlinkBackup = symlinkPath;
    symlinkBackup += ".bak";
    std::filesystem::create_symlink(
        directory.path()
            / "missing-backup-target",
        symlinkBackup);
    require(
        !talas::project::saveProjectFile(
             symlinkPath,
             makeDocument("Must not publish", 2))
             .ok()
            && !std::filesystem::exists(
                symlinkPath)
            && std::filesystem::is_symlink(
                symlinkBackup),
        "a dangling backup symlink without a primary should reject save without mutation");
#else
    // An atomic replace never leaves a backup behind, so a stray .bak (for
    // example from `sed -i.bak`) is someone else's file: save around it.
    TemporaryDirectory directory;
    const auto filePath = directory.path() / "file-backup.talas.json";
    auto fileBackup = filePath;
    fileBackup += ".bak";
    {
        std::ofstream output(fileBackup, std::ios::binary);
        output << "unrelated backup";
    }
    require(
        talas::project::saveProjectFile(filePath, makeDocument("Saved", 0)).ok()
            && readText(filePath).find("Saved") != std::string::npos
            && readText(fileBackup) == "unrelated backup",
        "a stray regular .bak must not block saving and must stay untouched");
    require(
        talas::project::saveProjectFile(filePath, makeDocument("Saved again", 1)).ok()
            && readText(filePath).find("Saved again") != std::string::npos
            && readText(fileBackup) == "unrelated backup",
        "replacing an existing project must also leave a stray .bak untouched");

    const auto directoryPath = directory.path() / "directory-backup.talas.json";
    auto directoryBackup = directoryPath;
    directoryBackup += ".bak";
    std::filesystem::create_directories(directoryBackup);
    {
        std::ofstream marker(directoryBackup / "keep.txt");
        marker << "preserve directory";
    }
    require(
        talas::project::saveProjectFile(directoryPath, makeDocument("Saved", 2)).ok()
            && readText(directoryBackup / "keep.txt") == "preserve directory",
        "a .bak directory must not block saving and must stay untouched");
#endif
}

void testSaveThroughSymlinkReplacesItsTarget()
{
    TemporaryDirectory directory;
    const auto target = directory.path() / "real" / "Project.talas.json";
    std::filesystem::create_directories(target.parent_path());
    require(talas::project::saveProjectFile(target, makeDocument("Initial", 0)).ok(),
        "symlink fixture target should save");
    const auto link = directory.path() / "Linked.talas.json";
    std::filesystem::create_symlink(std::filesystem::path{"real"} / "Project.talas.json", link);
    require(talas::project::saveProjectFile(link, makeDocument("Through link", 1)).ok(),
        "saving through a symlink should succeed");
    require(std::filesystem::is_symlink(link)
            && readText(target).find("Through link") != std::string::npos,
        "saving through a symlink must keep the link and replace the file it names");
}

void testSavePreservesProjectFilePermissions()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "Private.talas.json";
    require(talas::project::saveProjectFile(path, makeDocument("Initial", 0)).ok(),
        "permission fixture should save");
    const auto ownerOnly = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    std::filesystem::permissions(path, ownerOnly);
    require(talas::project::saveProjectFile(path, makeDocument("Replacement", 1)).ok(),
        "permission fixture should save again");
    require((std::filesystem::status(path).permissions() & std::filesystem::perms::all) == ownerOnly,
        "replacing a project must keep its permissions");
}

void testLegacyRampFileLoadsAndSavesAsVersionFourLinearRamp()
{
    TemporaryDirectory directory;
    const auto legacyPath = directory.path() / "legacy.talas.json";
    std::ofstream legacy(legacyPath);
    legacy << R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": { "title": "Legacy" },
  "graph": {
    "nodes": [{ "id": "ramp", "typeId": "talas.ramp", "parameters": [
      { "id": "duration", "expression": "1 s" },
      { "id": "start", "expression": "0" },
      { "id": "end", "expression": "1" }
    ] }],
    "connections": []
  }
})json";
    legacy.close();

    const auto loaded = talas::project::loadProjectFile(legacyPath);
    require(loaded.ok(), "legacy Ramp project file should load");
    require(loaded.document->formatVersion == talas::project::currentProjectFormatVersion, "legacy project file should migrate to the current version");
    require(loaded.document->graph.nodes[0].typeId == "talas.linearRamp", "legacy file Ramp should migrate to LinearRamp");

    const auto upgradedPath = directory.path() / "upgraded.talas.json";
    require(talas::project::saveProjectFile(upgradedPath, *loaded.document).ok(), "migrated project should save");
    std::ifstream upgraded(upgradedPath);
    const std::string text((std::istreambuf_iterator<char>(upgraded)), std::istreambuf_iterator<char>());
    require(text.find(R"json("version": 156)json") != std::string::npos, "upgraded project file should serialize the current version");
    require(text.find(R"json("typeId": "talas.linearRamp")json") != std::string::npos,
            "upgraded project file should serialize LinearRamp identity");
}

void testMissingComponentSaveAsAndCheckedConflictAreLossless()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.talas.json";
    const auto saveAs = directory.path() / "copy.talas.json";
    const auto document = makeMissingComponentDocument();
    const auto canonical =
        talas::project::serializeProjectDocument(document);

    const auto saved =
        talas::project::saveProjectFile(source, document);
    const auto opened =
        talas::project::loadProjectFile(source);
    require(
        saved.ok()
            && opened.ok()
            && opened.revision.has_value()
            && talas::project::serializeProjectDocument(
                   *opened.document)
                == canonical,
        "ordinary save/open should retain missing-component bytes exactly");

    require(
        talas::project::saveProjectFile(
            saveAs,
            *opened.document)
            .ok(),
        "Save As should accept a preserved missing component");
    const auto copied = talas::project::loadProjectFile(saveAs);
    require(
        copied.ok()
            && talas::project::serializeProjectDocument(
                   *copied.document)
                == canonical,
        "Save As reopen should retain capsule, state, and presentation exactly");

    const auto externalBytes = readText(source) + "\n ";
    {
        std::ofstream external(
            source,
            std::ios::binary | std::ios::trunc);
        external << externalBytes;
    }
    const auto conflicted =
        talas::project::saveProjectFile(
            source,
            document,
            opened.revision);
    require(
        !conflicted.ok()
            && conflicted.error->code
                == talas::project::ProjectFileErrorCode::ExternalConflict
            && readText(source) == externalBytes,
        "checked-save conflict should preserve external bytes and publish no partial missing-component document");
}

void testInvalidSampleMapsAreRejectedBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    std::vector<talas::project::ProjectDocument> invalidDocuments;

    auto invalidSchema = makeSampleMapDocument();
    invalidSchema.sampleMapLibrary.schemaVersion = 2;
    invalidDocuments.push_back(std::move(invalidSchema));

    auto duplicateIdentity = makeSampleMapDocument();
    duplicateIdentity.sampleMapLibrary.maps.push_back(
        duplicateIdentity.sampleMapLibrary.maps.front());
    invalidDocuments.push_back(std::move(duplicateIdentity));

    auto missingAsset = makeSampleMapDocument();
    missingAsset.sampleMapLibrary.maps.front()
        .regions.front().assetId = "missing_audio";
    invalidDocuments.push_back(std::move(missingAsset));

    auto invalidLoop = makeSampleMapDocument();
    invalidLoop.sampleMapLibrary.maps.front()
        .regions.front().sustainLoop->endFrameExclusive = 129;
    invalidDocuments.push_back(invalidLoop);

    for (std::size_t index = 0;
         index < invalidDocuments.size();
         ++index)
    {
        const auto parent = directory.path()
            / ("invalid-" + std::to_string(index));
        const auto path = parent / "Project.talas.json";
        const auto saved = talas::project::saveProjectFile(
            path,
            invalidDocuments[index]);
        require(
            !saved.ok()
                && saved.error->code
                    == talas::project::ProjectFileErrorCode::InvalidProject
                && !std::filesystem::exists(parent),
            "invalid sample-map family should fail before parent or target creation");
    }

    const auto existing = directory.path() / "existing.talas.json";
    require(
        talas::project::saveProjectFile(
            existing,
            makeSampleMapDocument()).ok(),
        "valid sample-map preservation fixture should save");
    const auto originalBytes = readText(existing);
    auto backup = existing;
    backup += ".bak";
    {
        std::ofstream output(backup, std::ios::binary);
        output << "owner recovery bytes";
    }
    const auto backupBytes = readText(backup);
    const auto rejected = talas::project::saveProjectFile(
        existing,
        invalidLoop);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && readText(existing) == originalBytes
            && readText(backup) == backupBytes,
        "invalid sample map should preserve an existing target and backup byte-exact");
}

void testInvalidMidiSourceMappingFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "invalid-midi";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    document.virtualControls.push_back({
        "cutoff",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        0.0,
        1.0,
        talas::project::MidiControlMapping{
            1,
            74,
            talas::project::MidiControlSourceMode::ExactInput,
            {},
        },
    });

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "invalid exact MIDI source should fail as InvalidProject before filesystem mutation");
}

void testInvalidScoreFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "invalid-score";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    document.timeline.score.tracks.push_back({
        "master",
        "Master",
        talas::project::ScoreTrackKind::Master,
        talas::project::ScoreTimeDomain::Seconds,
        {},
        2,
        talas::project::ScoreOverlapMode::Layer,
    });
    document.timeline.score.clips.push_back({
        "orphan",
        "missingTrack",
        "missingDefinition",
        {},
        {talas::project::ScoreTimeDomain::Seconds, 1.0, 0},
        {},
        {},
        {},
        1,
    });

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "invalid Score should return InvalidProject without throwing or mutating the filesystem");
}

void testInvalidGeneratorTargetFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "invalid-generator-target";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    talas::project::ProjectGeneratorProgram program;
    program.generatorId = "missingTarget";
    program.displayName = "Missing target";
    program.source.terminals = {"note"};
    program.source.grammar = "start:note";
    program.source.maximumRewritePasses = 1;
    program.source.maximumOutputSymbols = 1;
    talas::project::ProjectGeneratorTerminalBinding binding;
    binding.terminal = "note";
    binding.actions.push_back(
        talas::project::ProjectGeneratorScoreAutomationAction{
            "missingLane",
            talas::expr::ValueDomain::Scalar,
            0.5,
            talas::core::AutomationInterpolation::Step,
        });
    program.output.bindings.push_back(std::move(binding));
    document.generatorLibrary.programs.push_back(std::move(program));

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "invalid generator target should fail before parent or staging artifact creation");
}

void testInvalidPureExpressionFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "invalid-expression";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({
        "constant",
        "talas.constant",
        {{"value", std::string{"[1,2]"}}},
    });

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "invalid PureV2 expression should return InvalidProject before filesystem mutation");
}

void testInvalidVariableLibraryFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "invalid-variables";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    talas::project::ProjectVariableDeclaration declaration;
    declaration.variableId = "invalid-variable";
    declaration.displayName = "Invalid";
    document.variableLibrary.variables.push_back(std::move(declaration));

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "invalid variable authority should return InvalidProject before filesystem mutation");
}

void testPrematurePureV3AuthorityFailsBeforeFilesystemMutation()
{
    TemporaryDirectory directory;
    const auto parent = directory.path() / "premature-pure-v3";
    const auto path = parent / "Project.talas.json";

    talas::project::ProjectDocument document;
    document.formatVersion = 23;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::PureV3;

    const auto rejected = talas::project::saveProjectFile(path, document);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::project::ProjectFileErrorCode::InvalidProject
            && !std::filesystem::exists(parent),
        "format-23 PureV3 authority should fail before filesystem mutation");
}
}

void testSubnormalControlValueSavesAndLoads()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "subnormal.talas.json";
    auto document = makeDocument("Subnormal", 0);
    document.virtualControls.push_back({
        "Tiny", talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        std::numeric_limits<double>::denorm_min(), 0.0, 1.0});
    talas::project::reconcileProjectPresentation(document);

    const auto saved = talas::project::saveProjectFile(path, document);
    require(
        saved.ok(),
        "a subnormal control value must not block saving");
    const auto loaded = talas::project::loadProjectFile(path);
    require(
        loaded.ok()
            && loaded.document->virtualControls.back().value
                == std::numeric_limits<double>::denorm_min(),
        "a subnormal control value must load back exactly");
}

int main()
{
    testSaveLoadAndReplaceProjectFile();
    testCheckedSaveRejectsExternalContentAndFormattingChanges();
    testCheckedSaveRejectsDeletionAndCreateAfterMissing();
    testSaveRejectsDirectoryDestination();
    testLoadReportsIoAndParseFailures();
    testUnownedTemporaryArtifactsArePreserved();
    testUnownedBackupSymlinkIsPreserved();
    testBackupOnlyStatesDoNotBlockSave();
    testSaveThroughSymlinkReplacesItsTarget();
    testSavePreservesProjectFilePermissions();
    testLegacyRampFileLoadsAndSavesAsVersionFourLinearRamp();
    testMissingComponentSaveAsAndCheckedConflictAreLossless();
    testInvalidSampleMapsAreRejectedBeforeFilesystemMutation();
    testInvalidMidiSourceMappingFailsBeforeFilesystemMutation();
    testInvalidScoreFailsBeforeFilesystemMutation();
    testInvalidGeneratorTargetFailsBeforeFilesystemMutation();
    testInvalidPureExpressionFailsBeforeFilesystemMutation();
    testInvalidVariableLibraryFailsBeforeFilesystemMutation();
    testPrematurePureV3AuthorityFailsBeforeFilesystemMutation();
    testSubnormalControlValueSavesAndLoads();
    return 0;
}
