#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/audio/ProjectPackager.h"
#include "talas/cli/Cli.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectAutosaveState.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectRecoveryFile.h"
#include "talas/project/ProjectSampleMaps.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-m4-sample-map-workflow-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "file should be readable: " + path.string());
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

void copyFile(
    const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    std::filesystem::create_directories(destination.parent_path());
    std::error_code error;
    const auto copied = std::filesystem::copy_file(
        source,
        destination,
        std::filesystem::copy_options::overwrite_existing,
        error);
    require(copied && !error, "fixture copy failed: " + error.message());
}

struct CliResult
{
    int exitCode = -1;
    std::string output;
    std::string error;
};

CliResult runCli(std::vector<std::string> arguments)
{
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);
    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = talas::cli::runCli(views, output, error);
    return {exitCode, output.str(), error.str()};
}

void requireCliOk(CliResult result, std::string_view operation)
{
    require(
        result.exitCode == 0 && result.error.empty(),
        std::string{operation} + " failed: " + result.error);
    require(!result.output.empty(), std::string{operation} + " should report output");
}

void requireCanonicalSampleMap(
    const talas::project::ProjectDocument& document,
    std::string_view expectedBytes)
{
    require(
        document.formatVersion == talas::project::currentProjectFormatVersion
            && document.sampleMapLibrary.schemaVersion == 1
            && document.sampleMapLibrary.maps.size() == 1
            && document.sampleMapLibrary.maps.front().regions.size() == 2
            && document.assets.size() == 1,
        "fixture should retain one format-13 map, two regions, and one Audio asset");
    require(
        talas::project::projectSampleMapDiagnostics(document).empty(),
        "fixture sample-map authority should be internally valid");
    require(
        std::ranges::count(
            document.graph.nodes,
            std::string{"talas.sampleMapPlayer"},
            &talas::project::ProjectNode::typeId)
            == 1,
        "fixture should make its Audio asset reachable only through one sample-map player");
    const auto canonical = talas::project::serializeProjectDocument(document);
    require(
        canonical.find(R"json("version": 156)json")
                != std::string::npos
            && canonical != expectedBytes,
        "format-thirteen fixture should migrate to canonical format-32 bytes");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "migrated sample-map fixture should remain byte-stable");
}

void testOriginalSampleMapLifecycle()
{
    const auto repository = std::filesystem::path{TALAS_SOURCE_DIR};
    const auto example = repository / "examples/m4_sample_map_instrument.talas.json";
    const auto wav = repository / "examples/resources/m3-stream-source.wav";

    talas::project::ProjectDocument newDocument;
    require(
        newDocument.formatVersion == talas::project::currentProjectFormatVersion
            && newDocument.sampleMapLibrary.schemaVersion == 1
            && newDocument.sampleMapLibrary.maps.empty(),
        "New should begin at current format 35 with an empty version-1 sample-map library");

    auto source = readText(example);
    if (source.ends_with('\n'))
        source.pop_back();
    const auto opened = talas::project::loadProjectFile(example);
    require(opened.ok(), "Open should load the original sample-map example");
    requireCanonicalSampleMap(*opened.document, source);

    requireCliOk(
        runCli({"validate", "--project", example.string()}),
        "CLI validate of original sample-map example");
    requireCliOk(
        runCli({
            "render",
            "--project",
            example.string(),
            "--frames",
            "256",
            "--sample-rate",
            "8192",
            "--max-block-frames",
            "128",
            "--format",
            "json",
        }),
        "CLI render of original sample-map example");

    TemporaryDirectory directory;
    const auto savedProject = directory.path() / "opened/SampleMap.talas.json";
    const auto savedWav = savedProject.parent_path() / "resources/m3-stream-source.wav";
    copyFile(wav, savedWav);
    const auto firstSave = talas::project::saveProjectFile(savedProject, *opened.document);
    require(
        firstSave.ok() && firstSave.revision.has_value(),
        "Save should publish the canonical sample-map document and revision");
    const auto reopened = talas::project::loadProjectFile(savedProject);
    require(
        reopened.ok() && reopened.revision == firstSave.revision,
        "saved sample-map document should reopen at the exact disk revision");
    requireCanonicalSampleMap(*reopened.document, source);

    {
        std::ofstream external(savedProject, std::ios::binary | std::ios::app);
        external << "\n ";
    }
    const auto externalBytes = readText(savedProject);
    const auto conflicted = talas::project::saveProjectFile(
        savedProject,
        *reopened.document,
        reopened.revision);
    require(
        !conflicted.ok()
            && conflicted.error->code
                == talas::project::ProjectFileErrorCode::ExternalConflict
            && readText(savedProject) == externalBytes,
        "checked Save should reject an external project-file change without overwriting it");
    require(
        talas::project::saveProjectFile(savedProject, *reopened.document).ok(),
        "explicit conflict resolution should restore canonical project bytes");

    const auto saveAsProject = directory.path() / "saved-as/deeper/SampleMap.talas.json";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(
        *reopened.document,
        registry);
    require(
        editor.rebaseAssetPathsForProjectMove(savedProject, saveAsProject).ok(),
        "Save As should rebase the map-only reachable Audio path");
    require(
        talas::project::saveProjectFile(saveAsProject, editor.document()).ok(),
        "Save As should publish the rebased sample-map document");
    const auto saveAsOpened = talas::project::loadProjectFile(saveAsProject);
    require(
        saveAsOpened.ok()
            && talas::project::resolveProjectAssetPath(
                   saveAsOpened.document->assets.front(),
                   saveAsProject)
                == savedWav.lexically_normal(),
        "Save As/reopen should preserve the original accepted Audio target");

    talas::project::ProjectAutosaveState autosave(10.0, 10.0, 20.0);
    autosave.documentChanged(true, 100.0);
    require(
        autosave.shouldStartWrite(true, false, 110.0),
        "dirty format-13 sample-map state should reach its autosave deadline");
    autosave.writeStarted();
    talas::project::ProjectRecoveryRecord recovery;
    recovery.projectPath = saveAsProject;
    recovery.projectRevision = saveAsOpened.revision;
    recovery.cleanDocument = *reopened.document;
    require(
        talas::project::appendProjectRecoveryVersion(
            recovery,
            *saveAsOpened.document,
            1000),
        "autosave should append the complete sample-map document as one recovery version");
    const auto recoveryPath = directory.path() / "recovery/current.talas-recovery.json";
    const auto recoverySave = talas::project::saveProjectRecoveryFile(
        recoveryPath,
        recovery);
    autosave.writeFinished(recoverySave.ok(), true, 111.0);
    require(
        recoverySave.ok() && !autosave.pending() && !autosave.hasWriteFailure(),
        "successful sample-map autosave should clear pending/failure state");
    const auto recovered = talas::project::loadProjectRecoveryFile(recoveryPath);
    require(
        recovered.ok()
            && recovered.record->versions.size() == 1
            && recovered.record->versions.front().recoveredDocument
                   .sampleMapLibrary
                == saveAsOpened.document->sampleMapLibrary
            && recovered.record->versions.front().recoveredDocument.assets
                == saveAsOpened.document->assets,
        "recovery should retain the complete map and its accepted manifest identity");

    const auto watchBefore = talas::audio::captureProjectAssetWatchSnapshot(
        *saveAsOpened.document,
        registry,
        saveAsProject);
    require(
        watchBefore.entries.size() == 1,
        "map-only reachability should produce one external-change watcher entry");
    {
        std::ofstream changed(savedWav, std::ios::binary | std::ios::app);
        changed.put('\0');
    }
    const auto watchAfter = talas::audio::captureProjectAssetWatchSnapshot(
        *saveAsOpened.document,
        registry,
        saveAsProject);
    require(
        talas::audio::findProjectAssetWatchChange(watchBefore, watchAfter).has_value(),
        "external Audio changes should be detected for map-only reachable assets");
    copyFile(wav, savedWav);

    talas::audio::ProjectPackageOptions packageOptions;
    const auto package = directory.path() / "portable/SampleMap.talaspkg";
    packageOptions.outputPackagePath = package;
    packageOptions.sourceProjectFilePath = saveAsProject;
    const auto packaged = talas::audio::saveProjectPackage(
        *saveAsOpened.document,
        packageOptions);
    require(
        packaged.ok()
            && packaged.assetCount == 1
            && packaged.copiedFileCount == 1,
        "package should embed the one map-only reachable Audio payload exactly once");
    const auto resolvedPackage = talas::project::resolveProjectPackagePath(package);
    const auto packageOpened = resolvedPackage.ok()
        ? talas::project::loadProjectFile(resolvedPackage.projectFilePath)
        : talas::project::ProjectFileLoadResult{};
    require(
        resolvedPackage.ok()
            && packageOpened.ok()
            && talas::project::validateProjectPackageAssets(
                   *packageOpened.document,
                   resolvedPackage)
                   .ok()
            && packageOpened.document->sampleMapLibrary
                == saveAsOpened.document->sampleMapLibrary,
        "package should reopen with exact sample-map identities and verified embedded bytes");
    requireCliOk(
        runCli({"validate", "--project", package.string()}),
        "CLI validate of portable sample-map package");
    requireCliOk(
        runCli({
            "render",
            "--project",
            package.string(),
            "--frames",
            "256",
            "--sample-rate",
            "8192",
            "--max-block-frames",
            "128",
            "--format",
            "json",
        }),
        "CLI render of portable sample-map package");
}
}

int main()
{
    try
    {
        testOriginalSampleMapLifecycle();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
