#include "talas/audio/ProjectPackager.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
        : path_(std::filesystem::temp_directory_path()
                / ("talas-project-packager-tests-"
                   + std::to_string(
                       std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
    {
        std::filesystem::create_directories(path_);
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

void writeBytes(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "test fixture should write bytes");
}

void writeText(
    const std::filesystem::path& path,
    std::string_view text)
{
    writeBytes(
        path,
        std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::vector<std::uint8_t> readBytes(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

bool directoryContainsNameWithPrefix(
    const std::filesystem::path& directory,
    std::string_view prefix)
{
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(
             directory,
             error))
    {
        if (entry.path()
                .filename()
                .string()
                .starts_with(prefix))
        {
            return true;
        }
    }
    return false;
}

talas::project::ProjectDocument assetDocument(
    std::string title,
    std::vector<talas::project::ProjectAsset> assets)
{
    talas::project::ProjectDocument document;
    document.metadata.title = std::move(title);
    document.assets = std::move(assets);
    return document;
}

talas::project::ProjectDocument missingComponentDocument(
    std::string recoveryHint)
{
    talas::project::ProjectDocument document;
    document.expressionDefinitions.definitions.push_back({
        "half",
        {{"value", talas::expr::ValueDomain::Scalar}},
        talas::expr::ValueDomain::Scalar,
        "value * 0.5",
    });
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
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
    component.recoveryHint = std::move(recoveryHint);
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
        document.presentation.performanceControls[1].target, -0.25});
    document.presentation.performanceMeters.push_back(
        {"master-left", 1, {416, 96}, true});
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "missing1")
        ->position = {120, 80};
    return document;
}

void testPackagePathResolutionAndContainmentValidation()
{
    TemporaryDirectory directory;
    const auto plain = directory.path() / "plain.talas.json";
    const auto plainResolved =
        talas::project::resolveProjectPackagePath(plain);
    require(
        plainResolved.ok()
            && plainResolved.projectFilePath == plain
            && plainResolved.displayPath == plain
            && !plainResolved.packageDirectory.has_value(),
        "ordinary project paths should pass through unchanged");

    const auto package = directory.path() / "Portable.talaspkg";
    const auto assets = package
        / talas::project::projectPackageAssetsDirectoryName;
    std::filesystem::create_directories(assets);
    writeText(
        package / talas::project::projectPackageManifestFileName,
        talas::project::projectPackageManifestContents);
    require(
        talas::project::saveProjectFile(
            package / talas::project::projectPackageDocumentFileName,
            assetDocument("Portable", {}))
            .ok(),
        "package path fixture should save its project");

    const auto resolved =
        talas::project::resolveProjectPackagePath(package);
    require(
        resolved.ok()
            && resolved.packageDirectory
                == std::optional<std::filesystem::path>{package}
            && resolved.projectFilePath
                == package
                    / talas::project::projectPackageDocumentFileName
            && resolved.displayPath == package,
        "marked package directory should resolve to its internal project");
    const auto internalResolved =
        talas::project::resolveProjectPackagePath(
            resolved.projectFilePath);
    require(
        internalResolved.ok()
            && internalResolved.displayPath == package,
        "internal package project path should retain package display identity");
    require(
        talas::project::projectPackageDisplayPath(
            resolved.projectFilePath)
            == package
            && talas::project::projectPackageDisplayPath(plain)
                == plain,
        "package display paths should be derivable without filesystem access");

    auto contained = assetDocument(
        "Contained",
        {{"Hit",
          talas::project::ProjectAssetKind::Table,
          "Assets/hit.txt"}});
    writeText(assets / "hit.txt", "1 0 -1");
    require(
        talas::project::validateProjectPackageAssets(
            contained,
            resolved)
            .ok(),
        "package-relative asset below Assets should validate");

    contained.assets.front().path = "../outside.txt";
    require(
        !talas::project::validateProjectPackageAssets(
             contained,
             resolved)
             .ok(),
        "package asset traversal should be rejected");

    const auto outside = directory.path() / "outside.txt";
    writeText(outside, "outside");
    std::filesystem::create_symlink(
        outside,
        assets / "escape.txt");
    contained.assets.front().path = "Assets/escape.txt";
    require(
        !talas::project::validateProjectPackageAssets(
             contained,
             resolved)
             .ok(),
        "package asset symlink escape should be rejected");

    writeText(
        package / talas::project::projectPackageManifestFileName,
        "not a package");
    require(
        !talas::project::resolveProjectPackagePath(package).ok(),
        "malformed package manifest should be rejected");
}

void testPackageCopiesAllAssetsDeduplicatesAndRebases()
{
    TemporaryDirectory directory;
    const auto sourceProject =
        directory.path() / "source" / "Sound.talas.json";
    const auto table = directory.path() / "source" / "tables" / "hit.txt";
    const auto binary = directory.path() / "source" / "midi" / "part.mid";
    writeText(table, "1 0.5 -1");
    writeBytes(binary, {0x4d, 0x54, 0x68, 0x64});

    auto document = assetDocument(
        "Packaged",
        {
            {"Hit", talas::project::ProjectAssetKind::Table, "tables/hit.txt"},
            {"Part", talas::project::ProjectAssetKind::Binary, "midi/part.mid"},
            {"Hit Alias", talas::project::ProjectAssetKind::Table, "tables/hit.txt"},
        });
    const auto output = directory.path() / "Packaged.talaspkg";
    double lastProgress = -1.0;
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = output;
    options.sourceProjectFilePath = sourceProject;
    options.reportProgress = [&](double progress) {
        require(
            progress >= lastProgress && progress >= 0.0
                && progress <= 1.0,
            "package progress should be monotonic and bounded");
        lastProgress = progress;
    };

    const auto packaged =
        talas::audio::saveProjectPackage(document, options);
    require(packaged.ok(), "portable package should save: " + packaged.message);
    require(
        packaged.assetCount == 3
            && packaged.copiedFileCount == 2
            && packaged.copiedByteCount
                == readBytes(table).size() + readBytes(binary).size(),
        "package result should report registered and deduplicated copy counts");
    require(lastProgress == 1.0, "successful package should report complete progress");

    const auto resolved =
        talas::project::resolveProjectPackagePath(output);
    require(resolved.ok(), "published package should resolve");
    const auto loaded =
        talas::project::loadProjectFile(resolved.projectFilePath);
    require(loaded.ok(), "packaged project should load");
    require(
        talas::project::validateProjectPackageAssets(
            *loaded.document,
            resolved)
            .ok(),
        "packaged project assets should remain inside the package");
    require(
        loaded.document->assets.size() == 3
            && loaded.document->assets[0].path
                == loaded.document->assets[2].path
            && loaded.document->assets[0].path
                != loaded.document->assets[1].path
            && loaded.document->assets[0].disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && loaded.document->assets[1].disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && loaded.document->assets[0].contentHash.size() == 64
            && loaded.document->assets[0].contentHash
                == loaded.document->assets[2].contentHash
            && loaded.document->assets[0].generation == 1,
        "equal source paths should share one verified embedded file without changing generation");

    std::size_t fileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(
             output
             / talas::project::projectPackageAssetsDirectoryName))
    {
        if (entry.is_regular_file())
            ++fileCount;
    }
    require(fileCount == 2, "package Assets should contain only unique source files");

    const auto tamperedPath =
        output
        / talas::project::pathFromUtf8(
            loaded.document->assets.front().path);
    writeText(tamperedPath, "tampered bytes");
    require(
        !talas::project::validateProjectPackageAssets(
             *loaded.document,
             resolved)
             .ok(),
        "embedded package validation should reject content that no longer matches its manifest hash");
}

// The File Archivist archives a Sound file
// found on the disk, not one open: read from its file, it and a copy of each
// file it uses are saved together in the folder given, which plays without
// the originals; the Sound file and its files are left as they were.
void testArchivingASoundFileFoundOnTheDisk()
{
    TemporaryDirectory directory;
    const auto sourceProject = directory.path() / "Timelines" / "crispy mix.talas.json";
    const auto voice = directory.path() / "Timelines" / "samples" / "voice.txt";
    writeText(voice, "0.25 -0.5 1");
    require(talas::project::saveProjectFile(sourceProject,
                assetDocument("crispy mix", {{"Voice", talas::project::ProjectAssetKind::Table, "samples/voice.txt"}}))
                .ok(),
        "the Sound file to archive should save");
    const auto found = talas::project::loadProjectFile(sourceProject);
    require(found.ok(), "the File Archivist should read the Sound file from the disk");

    const auto archive = directory.path() / "toolsTutorial" / "crispy mix archive folder.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = archive;
    options.sourceProjectFilePath = sourceProject;
    const auto archived = talas::audio::saveProjectPackage(*found.document, options);
    require(archived.ok() && archived.assetCount == 1 && archived.copiedFileCount == 1,
        "the archive should hold the Sound file and a copy of the file it uses: " + archived.message);

    std::filesystem::rename(voice, directory.path() / "voice moved.txt");
    const auto resolved = talas::project::resolveProjectPackagePath(archive);
    const auto loaded = talas::project::loadProjectFile(resolved.projectFilePath);
    require(resolved.ok() && loaded.ok() && loaded.document->metadata.title == "crispy mix"
            && talas::project::validateProjectPackageAssets(*loaded.document, resolved).ok()
            && readBytes(archive / talas::project::pathFromUtf8(loaded.document->assets.front().path))
                == readBytes(directory.path() / "voice moved.txt"),
        "the archive should be self-sufficient, its copy the file the Sound used");
    const auto again = talas::project::loadProjectFile(sourceProject);
    require(again.ok() && again.document->assets.front().path == "samples/voice.txt",
        "the Sound file archived should be left as it was");
}

void testFailureCancellationAndInvalidDestinationPreserveState()
{
    TemporaryDirectory directory;
    const auto sourceProject =
        directory.path() / "source" / "Sound.talas.json";
    const auto source = directory.path() / "source" / "large.bin";
    writeBytes(source, std::vector<std::uint8_t>(2 * 1024 * 1024, 0x5a));
    auto document = assetDocument(
        "Initial",
        {{"Large", talas::project::ProjectAssetKind::Binary, "large.bin"}});
    const auto output = directory.path() / "Portable.talaspkg";

    talas::audio::ProjectPackageOptions initialOptions;
    initialOptions.outputPackagePath = output;
    initialOptions.sourceProjectFilePath = sourceProject;
    require(
        talas::audio::saveProjectPackage(document, initialOptions).ok(),
        "initial package fixture should publish");

    auto broken = document;
    broken.metadata.title = "Broken replacement";
    broken.assets.front().path = "missing.bin";
    const auto failed =
        talas::audio::saveProjectPackage(broken, initialOptions);
    require(!failed.ok(), "missing replacement asset should fail packaging");
    const auto afterFailure = talas::project::loadProjectFile(
        output / talas::project::projectPackageDocumentFileName);
    require(
        afterFailure.ok()
            && afterFailure.document->metadata.title == "Initial",
        "failed replacement should preserve the prior package");

    auto staleHash = document;
    staleHash.assets.front().contentHash.assign(64, '0');
    const auto mismatchedHash =
        talas::audio::saveProjectPackage(
            staleHash,
            initialOptions);
    const auto afterHashFailure =
        talas::project::loadProjectFile(
            output
            / talas::project::projectPackageDocumentFileName);
    require(
        !mismatchedHash.ok()
            && afterHashFailure.ok()
            && afterHashFailure.document->metadata.title == "Initial",
        "packaging should fail atomically when referenced bytes differ from the accepted manifest hash");

    auto replacement = document;
    replacement.metadata.title = "Replacement";
    const auto replaced =
        talas::audio::saveProjectPackage(
            replacement,
            initialOptions);
    const auto afterReplacement =
        talas::project::loadProjectFile(
            output
            / talas::project::
                projectPackageDocumentFileName);
    require(
        replaced.ok()
            && afterReplacement.ok()
            && afterReplacement.document->metadata.title
                == "Replacement"
            && !std::filesystem::exists(
                std::filesystem::path{
                    output.string() + ".bak"}),
        "successful replacement should publish the new package and remove its backup");

    auto invalidated = document;
    invalidated.metadata.title = "Invalidated replacement";
    int admissionChecks = 0;
    auto invalidatedOptions = initialOptions;
    invalidatedOptions.admitPublication = [&] {
        ++admissionChecks;
        return false;
    };
    const auto invalidatedResult =
        talas::audio::saveProjectPackage(
            invalidated,
            invalidatedOptions);
    const auto afterInvalidation =
        talas::project::loadProjectFile(
            output
            / talas::project::
                projectPackageDocumentFileName);
    require(
        invalidatedResult.cancelled()
            && admissionChecks == 1
            && afterInvalidation.ok()
            && afterInvalidation.document->metadata.title
                == "Replacement",
        "terminal authority rejection must preserve the prior package byte authority");

    bool cancel = false;
    talas::audio::ProjectPackageOptions cancelledOptions;
    cancelledOptions.outputPackagePath =
        directory.path() / "Cancelled.talaspkg";
    cancelledOptions.sourceProjectFilePath = sourceProject;
    cancelledOptions.shouldCancel = [&] { return cancel; };
    cancelledOptions.reportProgress = [&](double progress) {
        if (progress > 0.0)
            cancel = true;
    };
    const auto cancelled =
        talas::audio::saveProjectPackage(document, cancelledOptions);
    require(cancelled.cancelled(), "copy cancellation should be reported");

    int hashCancellationChecks = 0;
    talas::audio::ProjectPackageOptions hashCancelledOptions;
    hashCancelledOptions.outputPackagePath =
        directory.path() / "Hash-Cancelled.talaspkg";
    hashCancelledOptions.sourceProjectFilePath = sourceProject;
    hashCancelledOptions.shouldCancel = [&] {
        return ++hashCancellationChecks >= 3;
    };
    const auto hashCancelled =
        talas::audio::saveProjectPackage(
            document,
            hashCancelledOptions);
    require(
        hashCancelled.cancelled()
            && !std::filesystem::exists(
                hashCancelledOptions.outputPackagePath),
        "pre-copy hash cancellation should report cancelled without publishing a package");

    auto duplicateManifest = document;
    duplicateManifest.assets.push_back(
        duplicateManifest.assets.front());
    duplicateManifest.assets.back().generation = 2;
    auto duplicateOptions = initialOptions;
    duplicateOptions.outputPackagePath =
        directory.path() / "Duplicate.talaspkg";
    const auto duplicate =
        talas::audio::saveProjectPackage(
            duplicateManifest,
            duplicateOptions);
    require(
        !duplicate.ok()
            && duplicate.error->code
                == talas::audio::ProjectPackageErrorCode::InvalidInput
            && !std::filesystem::exists(
                duplicateOptions.outputPackagePath),
        "packaging should reject duplicate active asset identities before publication");
    require(
        !std::filesystem::exists(
            cancelledOptions.outputPackagePath)
            && !directoryContainsNameWithPrefix(
                directory.path(),
                "Cancelled.talaspkg.tmp-"),
        "cancelled package should remove staging and publish nothing");

    const auto invalid = directory.path() / "Invalid.talaspkg";
    std::filesystem::create_directories(invalid);
    writeText(invalid / "keep.txt", "keep");
    auto invalidOptions = initialOptions;
    invalidOptions.outputPackagePath = invalid;
    const auto invalidResult =
        talas::audio::saveProjectPackage(document, invalidOptions);
    require(
        !invalidResult.ok()
            && readBytes(invalid / "keep.txt")
                == std::vector<std::uint8_t>{'k', 'e', 'e', 'p'},
        "unmarked destination directory should be rejected without mutation");

    const auto trappedOutput =
        directory.path() / "SymlinkTrap.talaspkg";
    const auto sentinel =
        directory.path() / "sentinel";
    writeText(sentinel / "keep.txt", "keep");
    std::filesystem::create_directory_symlink(
        sentinel,
        std::filesystem::path{
            trappedOutput.string() + ".tmp"});
    auto trappedOptions = initialOptions;
    trappedOptions.outputPackagePath = trappedOutput;
    require(
        talas::audio::saveProjectPackage(
            document,
            trappedOptions)
            .ok()
            && readBytes(sentinel / "keep.txt")
                == std::vector<std::uint8_t>{'k', 'e', 'e', 'p'}
            && std::filesystem::is_symlink(
                std::filesystem::path{
                    trappedOutput.string() + ".tmp"}),
        "temporary package symlinks should be preserved without touching their targets");

    const auto staleOutput =
        directory.path() / "StaleSidecar.talaspkg";
    const auto staleTemporary =
        std::filesystem::path{
            staleOutput.string() + ".tmp"};
    writeText(
        staleTemporary / "unrelated.txt",
        "preserve");
    auto staleOptions = initialOptions;
    staleOptions.outputPackagePath = staleOutput;
    require(
        talas::audio::saveProjectPackage(
            document,
            staleOptions)
            .ok()
            && readBytes(
                staleTemporary
                / "unrelated.txt")
                == std::vector<std::uint8_t>{
                    'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'},
        "unowned temporary sidecar directories must never be deleted");

    const auto aliasParent =
        directory.path() / "source-alias";
    std::filesystem::create_directory_symlink(
        output.parent_path(),
        aliasParent);
    auto aliasOptions = initialOptions;
    aliasOptions.sourceProjectFilePath =
        output
        / talas::project::
            projectPackageDocumentFileName;
    aliasOptions.outputPackagePath =
        aliasParent / output.filename();
    const auto aliasResult =
        talas::audio::saveProjectPackage(
            replacement,
            aliasOptions);
    require(
        !aliasResult.ok()
            && talas::project::loadProjectFile(
                   output
                   / talas::project::
                       projectPackageDocumentFileName)
                   .ok(),
        "filesystem aliases of the active source package must be rejected");
}

void testInvalidSampleMapsCannotPublishOrReplacePackages()
{
    TemporaryDirectory directory;
    const auto sourceProject =
        directory.path() / "source" / "Sound.talas.json";
    const auto sourceAsset =
        directory.path() / "source" / "table.txt";
    writeText(sourceAsset, "0.25 0.5 0.75");
    auto document = assetDocument(
        "Package validation",
        {{"Table",
          talas::project::ProjectAssetKind::Table,
          "table.txt"}});
    const auto output =
        directory.path() / "Validated.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = output;
    options.sourceProjectFilePath = sourceProject;
    require(
        talas::audio::saveProjectPackage(
            document,
            options).ok(),
        "valid package baseline should publish");

    const auto resolved =
        talas::project::resolveProjectPackagePath(output);
    require(resolved.ok(), "valid package baseline should resolve");
    const auto projectBytes = readBytes(
        resolved.projectFilePath);
    const auto packaged = talas::project::loadProjectFile(
        resolved.projectFilePath);
    require(
        packaged.ok() && packaged.document->assets.size() == 1,
        "valid package baseline project should reopen");
    const auto assetBytes = readBytes(
        output
        / talas::project::pathFromUtf8(
            packaged.document->assets.front().path));

    document.sampleMapLibrary.schemaVersion = 2;
    const auto rejectedReplacement =
        talas::audio::saveProjectPackage(
            document,
            options);
    require(
        !rejectedReplacement.ok()
            && rejectedReplacement.error->code
                == talas::audio::ProjectPackageErrorCode::InvalidInput
            && readBytes(resolved.projectFilePath) == projectBytes
            && readBytes(
                   output
                   / talas::project::pathFromUtf8(
                       packaged.document->assets.front().path))
                == assetBytes
            && !std::filesystem::exists(
                output.string() + ".bak"),
        "invalid sample map should preserve an existing package byte-exact");

    auto newOptions = options;
    newOptions.outputPackagePath = directory.path()
        / "missing" / "Invalid.talaspkg";
    const auto rejectedNew =
        talas::audio::saveProjectPackage(
            document,
            newOptions);
    require(
        !rejectedNew.ok()
            && rejectedNew.error->code
                == talas::audio::ProjectPackageErrorCode::InvalidInput
            && !std::filesystem::exists(
                newOptions.outputPackagePath.parent_path()),
        "invalid sample map should fail before package parent or staging creation");
}

void testPackageImportIsAtomicAndCollisionSafe()
{
    TemporaryDirectory directory;
    const auto package = directory.path() / "Editable.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = package;
    require(
        talas::audio::saveProjectPackage(
            assetDocument("Editable", {}),
            options)
            .ok(),
        "empty package fixture should publish");

    const auto firstSource = directory.path() / "one" / "sample.wav";
    const auto secondSource = directory.path() / "two" / "sample.wav";
    writeBytes(firstSource, {1, 2, 3});
    writeBytes(secondSource, {4, 5, 6});

    const auto first = talas::audio::importProjectPackageAsset(
        package,
        firstSource,
        "Sample");
    require(first.ok() && first.created, "first package import should create a file");
    const auto repeated = talas::audio::importProjectPackageAsset(
        package,
        firstSource,
        "Sample");
    require(
        repeated.ok() && !repeated.created
            && repeated.relativePath == first.relativePath,
        "identical repeated import should reuse its package file");
    const auto alreadyPackaged =
        talas::audio::importProjectPackageAsset(
            package,
            package
                / talas::project::pathFromUtf8(
                    first.relativePath),
            "Renamed");
    require(
        alreadyPackaged.ok()
            && !alreadyPackaged.created
            && alreadyPackaged.relativePath
                == first.relativePath,
        "selecting an existing package asset should retain its current path");
    const auto collision = talas::audio::importProjectPackageAsset(
        package,
        secondSource,
        "Sample");
    require(
        collision.ok() && collision.created
            && collision.relativePath != first.relativePath,
        "different bytes with the same preferred name should use a collision suffix");
    require(
        readBytes(
            package / talas::project::pathFromUtf8(first.relativePath))
            == std::vector<std::uint8_t>({1, 2, 3}),
        "collision handling must not overwrite the first import");

    const auto staleImport =
        package / "Assets" / "Preserve.wav.tmp";
    writeText(
        staleImport / "unrelated.txt",
        "preserve");
    const auto preservedImport =
        talas::audio::importProjectPackageAsset(
            package,
            secondSource,
            "Preserve");
    require(
        preservedImport.ok()
            && readBytes(
                staleImport
                / "unrelated.txt")
                == std::vector<std::uint8_t>{
                    'p', 'r', 'e', 's', 'e', 'r', 'v', 'e'},
        "unowned import sidecars must never be deleted");

    const auto racingSource =
        directory.path() / "racing.wav";
    writeBytes(
        racingSource,
        std::vector<std::uint8_t>(
            32 * 1024 * 1024,
            0x7a));
    const auto racingDestination =
        package / "Assets" / "Racing.wav";
    std::atomic<bool> stopRacer = false;
    std::thread racer(
        [&] {
            while (!stopRacer.load(
                std::memory_order_acquire))
            {
                std::error_code error;
                for (const auto& entry :
                     std::filesystem::
                         directory_iterator(
                             package / "Assets",
                             error))
                {
                    const auto name =
                        entry.path()
                            .filename()
                            .string();
                    if (name.starts_with(
                            "Racing.wav.tmp"))
                    {
                        writeBytes(
                            racingDestination,
                            {9, 8, 7});
                        return;
                    }
                }
                std::this_thread::yield();
            }
        });
    struct StopAndJoinRacer
    {
        std::atomic<bool>& stop;
        std::thread& thread;

        ~StopAndJoinRacer()
        {
            stop.store(
                true,
                std::memory_order_release);
            if (thread.joinable())
            {
                thread.join();
            }
        }
    };
    const StopAndJoinRacer racerGuard{
        stopRacer,
        racer};
    const auto racingImport =
        talas::audio::importProjectPackageAsset(
            package,
            racingSource,
            "Racing");
    stopRacer.store(
        true,
        std::memory_order_release);
    racer.join();
    require(
        racingImport.ok()
            && racingImport.relativePath
                != "Assets/Racing.wav"
            && readBytes(racingDestination)
                == std::vector<std::uint8_t>{9, 8, 7},
        "a concurrent destination creator must not be overwritten");
}

void testPackageImportCancellationIsBoundedAndCleansStaging()
{
    TemporaryDirectory directory;
    const auto package = directory.path() / "Cancellation.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = package;
    require(
        talas::audio::saveProjectPackage(
            assetDocument("Cancellation", {}),
            options).ok(),
        "cancellation package fixture should publish");

    const auto source = directory.path() / "large-source.wav";
    {
        std::ofstream output(source, std::ios::binary | std::ios::trunc);
        output.seekp(64 * 1024 * 1024 - 1);
        output.put('\0');
        require(output.good(), "large sparse import source should be writable");
    }
    auto stagingChecks = 0;
    const auto started = std::chrono::steady_clock::now();
    const auto cancelled = talas::audio::importProjectPackageAsset(
        package,
        source,
        "Cancel",
        [&] {
            if (directoryContainsNameWithPrefix(
                    package / "Assets",
                    "Cancel.wav.tmp"))
            {
                return ++stagingChecks >= 2;
            }
            return false;
        });
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(
        !cancelled.ok()
            && cancelled.cancelled()
            && elapsed < std::chrono::seconds(2)
            && !std::filesystem::exists(package / "Assets" / "Cancel.wav")
            && !directoryContainsNameWithPrefix(
                package / "Assets",
                "Cancel.wav.tmp")
            && std::filesystem::is_regular_file(source),
        "package import cancellation should stop within one copy chunk and remove only owned staging output");
}

void testPackageImportCancellationRetainsFailedCleanupForRetry()
{
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory directory;
    const auto package = directory.path() / "Cleanup-Retry.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = package;
    require(
        talas::audio::saveProjectPackage(
            assetDocument("Cleanup Retry", {}),
            options).ok(),
        "cleanup-retry package fixture should publish");

    const auto source = directory.path() / "cleanup-source.wav";
    writeBytes(source, std::vector<std::uint8_t>(2 * 1024 * 1024, 0x4d));
    const auto assetsDirectory = package / "Assets";
    auto permissionsRestricted = false;
    auto result = talas::audio::importProjectPackageAsset(
        package,
        source,
        "Cleanup",
        [&] {
            if (!permissionsRestricted
                && directoryContainsNameWithPrefix(
                    assetsDirectory,
                    "Cleanup.wav.tmp"))
            {
                std::error_code error;
                std::filesystem::permissions(
                    assetsDirectory,
                    std::filesystem::perms::owner_read
                        | std::filesystem::perms::owner_exec,
                    std::filesystem::perm_options::replace,
                    error);
                require(!error, "test should restrict Assets directory permissions");
                permissionsRestricted = true;
                return true;
            }
            return false;
        });

    require(
        permissionsRestricted
            && !result.ok()
            && !result.cancelled()
            && result.errorCode
                == talas::audio::ProjectPackageImportErrorCode::CleanupFailed
            && !result.retainedStagingPath.empty()
            && std::filesystem::exists(result.retainedStagingPath),
        "failed cancellation cleanup should be typed and retain its owned staging path");

    std::error_code permissionsError;
    std::filesystem::permissions(
        assetsDirectory,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        permissionsError);
    require(!permissionsError, "test should restore Assets directory permissions");
    require(
        talas::audio::discardProjectPackageImportArtifacts(result).empty()
            && result.retainedStagingPath.empty()
            && !directoryContainsNameWithPrefix(
                assetsDirectory,
                "Cleanup.wav.tmp"),
        "retained import staging ownership should support an explicit cleanup retry");
#endif
}

void testPackagePreservesMissingComponentAsDataOnly()
{
    TemporaryDirectory directory;
    const auto executableHint =
        directory.path() / "Example.dylib";
    writeBytes(executableHint, {0xca, 0xfe, 0xba, 0xbe});
    const auto document = missingComponentDocument(
        talas::project::pathToUtf8(executableHint));
    const auto canonicalBefore =
        talas::project::serializeProjectDocument(document);

    const auto package = directory.path() / "Missing.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = package;
    const auto result =
        talas::audio::saveProjectPackage(document, options);
    require(
        result.ok()
            && result.assetCount == 0
            && result.copiedFileCount == 0,
        "missing-component package should contain no executable or asset payload: "
            + result.message);

    const auto resolved =
        talas::project::resolveProjectPackagePath(package);
    const auto reopened = resolved.ok()
        ? talas::project::loadProjectFile(
            resolved.projectFilePath)
        : talas::project::ProjectFileLoadResult{};
    require(
        resolved.ok()
            && reopened.ok()
            && talas::project::serializeProjectDocument(
                   *reopened.document)
                == canonicalBefore,
        "package reopen should preserve missing-node capsule and opaque state exactly");

    std::size_t regularFileCount = 0;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(package))
    {
        if (!entry.is_regular_file())
            continue;
        ++regularFileCount;
        require(
            entry.path().filename() != executableHint.filename(),
            "recoveryHint text must never become an executable package payload");
    }
    require(
        regularFileCount == 2
            && readBytes(executableHint)
                == std::vector<std::uint8_t>{
                    0xca,
                    0xfe,
                    0xba,
                    0xbe},
        "data-only package should contain only its manifest and canonical project while leaving the hinted file untouched");
}

// The Workbench packages and imports on std::thread workers whose default
// stack is far smaller than the main thread's; the copy path must not depend
// on a large stack buffer.
void testPackagingSucceedsOnADefaultWorkerThreadStack()
{
    TemporaryDirectory directory;
    const auto sourceProject =
        directory.path() / "source" / "Sound.talas.json";
    const auto table = directory.path() / "source" / "tables" / "hit.txt";
    writeText(table, "1 0.5 -1");
    const auto document = assetDocument(
        "Threaded",
        {{"Hit", talas::project::ProjectAssetKind::Table, "tables/hit.txt"}});
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = directory.path() / "Threaded.talaspkg";
    options.sourceProjectFilePath = sourceProject;

    talas::audio::ProjectPackageResult packaged;
    std::thread worker([&] { packaged = talas::audio::saveProjectPackage(document, options); });
    worker.join();
    require(packaged.ok(),
            "packaging on a default worker thread stack should succeed: " + packaged.message);

    talas::audio::ProjectPackageImportResult imported;
    std::thread importer([&] {
        imported = talas::audio::importProjectPackageAsset(
            options.outputPackagePath, table, "hit-copy.txt");
    });
    importer.join();
    require(imported.ok(),
            "importing on a default worker thread stack should succeed: " + imported.error);
}
}

int main()
{
    testPackagePathResolutionAndContainmentValidation();
    testPackageCopiesAllAssetsDeduplicatesAndRebases();
    testArchivingASoundFileFoundOnTheDisk();
    testFailureCancellationAndInvalidDestinationPreserveState();
    testInvalidSampleMapsCannotPublishOrReplacePackages();
    testPackageImportIsAtomicAndCollisionSafe();
    testPackageImportCancellationIsBoundedAndCleansStaging();
    testPackageImportCancellationRetainsFailedCleanupForRetry();
    testPackagePreservesMissingComponentAsDataOnly();
    testPackagingSucceedsOnADefaultWorkerThreadStack();
    return 0;
}
