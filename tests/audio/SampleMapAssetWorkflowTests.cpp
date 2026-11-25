#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/audio/ProjectAssetDisposition.h"
#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/ProjectPackager.h"
#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireOk(
    const talas::project::EditResult& result,
    const std::string& message)
{
    require(result.ok(), message + ": " + result.message);
}

void requireError(
    const talas::project::EditResult& result,
    talas::project::EditError expected,
    const std::string& message)
{
    require(!result.ok(), message + " should fail");
    require(result.error.has_value(), message + " should report an error");
    require(
        *result.error == expected,
        message + " should report the expected error");
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-sample-map-asset-workflow-tests-"
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

talas::core::NodeRegistry& builtinRegistry()
{
    static talas::core::NodeRegistry registry = [] {
        talas::core::NodeRegistry builtins;
        talas::core::registerBuiltinNodeTypes(builtins);
        return builtins;
    }();
    return registry;
}

void writeText(
    const std::filesystem::path& path,
    std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(output.good(), "text fixture should be writable");
}

bool packageHasImportStaging(const std::filesystem::path& package)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(
             package / talas::project::projectPackageAssetsDirectoryName,
             error))
    {
        if (entry.path().filename().string().find(".tmp")
            != std::string::npos)
            return true;
    }
    return false;
}

void writeMono(
    const std::filesystem::path& path,
    double value,
    std::size_t frameCount = 128)
{
    std::filesystem::create_directories(path.parent_path());
    const std::vector<std::vector<double>> channels{
        std::vector<double>(frameCount, value),
    };
    std::string error;
    require(
        talas::audio::writePcm16Wav(path, 48000.0, channels, error),
        "WAV fixture should be writable: " + error);
}

talas::project::ProjectAsset inspectedAudio(
    const std::filesystem::path& path,
    std::string id)
{
    talas::project::ProjectAsset manifest;
    manifest.id = std::move(id);
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.path = talas::project::pathToUtf8(path);
    const auto inspected = talas::project::inspectProjectAsset(
        manifest,
        path);
    require(inspected.ok(), "audio fixture should inspect");
    return inspected.asset;
}

std::filesystem::path createPackage(
    const std::filesystem::path& root)
{
    const auto package = root / "Disposition.talaspkg";
    std::filesystem::create_directories(
        package / talas::project::projectPackageAssetsDirectoryName);
    writeText(
        package / talas::project::projectPackageManifestFileName,
        talas::project::projectPackageManifestContents);
    require(
        talas::project::saveProjectFile(
            package / talas::project::projectPackageDocumentFileName,
            {})
            .ok(),
        "package fixture should save");
    return package;
}

talas::project::ProjectDocument sampleMapDocument(
    const talas::project::ProjectAsset& asset)
{
    talas::project::ProjectDocument document;
    document.assets.push_back(asset);
    talas::project::ProjectSampleRegion region;
    region.regionId = "voiceRegion";
    region.assetId = asset.id;
    region.rootNote = 60;
    region.lowNote = 0;
    region.highNote = 127;
    talas::project::ProjectSampleMap map;
    map.mapId = "voiceMap";
    map.displayName = "Voice";
    map.regions.push_back(std::move(region));
    document.sampleMapLibrary.maps.push_back(std::move(map));
    return document;
}

void addRawSampleMapPlayer(talas::project::ProjectDocument& document)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = "sampleMapId";
    parameter.text = "voiceMap";
    document.graph.nodes.push_back({
        "player",
        "talas.sampleMapPlayer",
        {std::move(parameter)},
        {},
    });
}

void testDispositionCandidatesPreserveIdentityAndUndo()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "external" / "voice.wav";
    writeMono(source, 0.25);
    const auto asset = inspectedAudio(source, "Samples/voice");
    const auto document = sampleMapDocument(asset);
    const auto package = createPackage(directory.path());

    talas::audio::ProjectAssetDispositionOptions embed;
    embed.assetId = asset.id;
    embed.targetDisposition =
        talas::project::ProjectAssetDisposition::Embedded;
    embed.projectFilePath = package;
    auto prepared = talas::audio::prepareProjectAssetDisposition(
        document,
        embed);
    require(
        prepared.ok()
            && prepared.candidate.asset.id == asset.id
            && prepared.candidate.asset.contentHash == asset.contentHash
            && prepared.candidate.asset.generation == asset.generation
            && prepared.candidate.asset.disposition
                == talas::project::ProjectAssetDisposition::Embedded
            && prepared.candidate.asset.path.starts_with("Assets/")
            && prepared.candidate.createdFiles.size() == 1,
        "Embed should return one verified same-generation disposition candidate");

    talas::project::ProjectEditor editor(document, builtinRegistry());
    requireOk(
        editor.acceptProjectAssetDisposition(prepared.candidate.asset),
        "publishing embedded disposition through ProjectEditor");
    require(
        editor.document().sampleMapLibrary.maps.front().regions.front().assetId
                == asset.id
            && editor.document().assets.front().disposition
                == talas::project::ProjectAssetDisposition::Embedded,
        "Embed should preserve sample-region identity and change only asset location policy");
    requireOk(editor.undo(), "Embed disposition should expose one undo");
    require(
        editor.document().assets == document.assets
            && editor.document().sampleMapLibrary
                == document.sampleMapLibrary,
        "Embed undo should restore exact project state without deleting bytes");
    requireError(
        editor.undo(),
        talas::project::EditError::NoUndoAvailable,
        "Embed disposition should not expose a second undo");

    auto tampered = prepared.candidate.asset;
    tampered.contentHash = std::string(64, 'f');
    requireError(
        editor.acceptProjectAssetDisposition(std::move(tampered)),
        talas::project::EditError::InvalidAsset,
        "disposition candidate that changes accepted content");

    talas::project::ProjectEditor staleEditor(document, builtinRegistry());
    auto concurrentlyRelocated = asset;
    concurrentlyRelocated.path = "alternate/voice.wav";
    requireOk(
        staleEditor.acceptProjectAssetDisposition(
            concurrentlyRelocated,
            asset),
        "establishing a newer asset location revision");
    requireError(
        staleEditor.acceptProjectAssetDisposition(
            prepared.candidate.asset,
            asset),
        talas::project::EditError::StaleAssetDisposition,
        "background disposition publication bound to an older asset revision");
}

void testReferenceRequiresExplicitExactBytes()
{
    TemporaryDirectory directory;
    const auto original = directory.path() / "original.wav";
    const auto exact = directory.path() / "durable" / "exact.wav";
    const auto changed = directory.path() / "durable" / "changed.wav";
    writeMono(original, 0.1);
    std::filesystem::create_directories(exact.parent_path());
    std::filesystem::copy_file(original, exact);
    writeMono(changed, 0.2);
    const auto asset = inspectedAudio(original, "Samples/voice");
    auto document = sampleMapDocument(asset);
    document.assets.front().disposition =
        talas::project::ProjectAssetDisposition::Embedded;

    talas::audio::ProjectAssetDispositionOptions missingPath;
    missingPath.assetId = asset.id;
    missingPath.targetDisposition =
        talas::project::ProjectAssetDisposition::Referenced;
    require(
        !talas::audio::prepareProjectAssetDisposition(
             document,
             missingPath)
             .ok(),
        "Reference should require an explicit durable path");

    auto reference = missingPath;
    reference.selectedPath = exact;
    reference.projectFilePath = directory.path() / "Project.talas.json";
    const auto prepared = talas::audio::prepareProjectAssetDisposition(
        document,
        reference);
    require(
        prepared.ok()
            && prepared.candidate.asset.id == asset.id
            && prepared.candidate.asset.generation == asset.generation
            && prepared.candidate.asset.disposition
                == talas::project::ProjectAssetDisposition::Referenced,
        "Reference should accept explicit exact bytes without changing generation");

    reference.selectedPath = changed;
    const auto rejected = talas::audio::prepareProjectAssetDisposition(
        document,
        reference);
    require(
        !rejected.ok()
            && rejected.error->code
                == talas::audio::ProjectAssetDispositionErrorCode::InspectionFailed,
        "Reference should reject selected bytes that differ from accepted content");

    const auto package = createPackage(directory.path());
    const auto managed = package
        / talas::project::projectPackageAssetsDirectoryName
        / "voice.wav";
    std::filesystem::copy_file(original, managed);
    auto embeddedDocument = document;
    embeddedDocument.assets.front().path = "Assets/voice.wav";
    embeddedDocument.assets.front().disposition =
        talas::project::ProjectAssetDisposition::Embedded;
    reference.projectFilePath = package;
    reference.selectedPath = managed;
    const auto internal = talas::audio::prepareProjectAssetDisposition(
        embeddedDocument,
        reference);
    require(
        !internal.ok()
            && internal.error->code
                == talas::audio::ProjectAssetDispositionErrorCode::InvalidInput,
        "Reference should reject a managed path inside the open project package");
}

void testEmbedDispositionCancellationCleansOwnedStaging()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "outside" / "cancel.wav";
    writeMono(source, 0.2);
    const auto asset = inspectedAudio(source, "Samples/cancel");
    const auto document = sampleMapDocument(asset);
    const auto package = createPackage(directory.path());

    auto stagingChecks = 0;
    talas::audio::ProjectAssetDispositionOptions embed;
    embed.assetId = asset.id;
    embed.targetDisposition =
        talas::project::ProjectAssetDisposition::Embedded;
    embed.projectFilePath = package;
    embed.shouldCancel = [&] {
        if (packageHasImportStaging(package))
            return ++stagingChecks >= 2;
        return false;
    };
    const auto cancelled = talas::audio::prepareProjectAssetDisposition(
        document,
        embed);
    require(
        !cancelled.ok()
            && cancelled.cancelled()
            && !packageHasImportStaging(package)
            && std::filesystem::is_regular_file(source),
        "Embed disposition cancellation should propagate through copy and remove owned staging only");
}

void testEmbedDispositionRetainsFailedImportCleanupForRetry()
{
#if defined(_WIN32)
    return;
#else
    TemporaryDirectory directory;
    const auto source = directory.path() / "outside" / "cleanup.wav";
    writeMono(source, 0.2, 1024 * 1024);
    const auto asset = inspectedAudio(source, "Samples/cleanup");
    const auto document = sampleMapDocument(asset);
    const auto package = createPackage(directory.path());
    const auto assetsDirectory =
        package / talas::project::projectPackageAssetsDirectoryName;

    auto permissionsRestricted = false;
    talas::audio::ProjectAssetDispositionOptions embed;
    embed.assetId = asset.id;
    embed.targetDisposition =
        talas::project::ProjectAssetDisposition::Embedded;
    embed.projectFilePath = package;
    embed.shouldCancel = [&] {
        if (!permissionsRestricted && packageHasImportStaging(package))
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
    };
    auto failed = talas::audio::prepareProjectAssetDisposition(
        document,
        embed);
    require(
        permissionsRestricted
            && !failed.ok()
            && !failed.cancelled()
            && failed.error->code
                == talas::audio::ProjectAssetDispositionErrorCode::CleanupFailed
            && failed.candidate.cleanupImports.size() == 1
            && !failed.candidate.cleanupImports.front()
                    .retainedStagingPath.empty(),
        "Embed should propagate failed package-import cleanup with retry ownership");

    std::error_code permissionsError;
    std::filesystem::permissions(
        assetsDirectory,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace,
        permissionsError);
    require(!permissionsError, "test should restore Assets directory permissions");
    require(
        talas::audio::discardPreparedProjectAssetDispositionFiles(
            failed.candidate).empty()
            && failed.candidate.cleanupImports.empty()
            && !packageHasImportStaging(package),
        "Embed cleanup retry should consume retained package-import ownership");
#endif
}

void testRelinkPreservesSampleRegionAndGeneration()
{
    TemporaryDirectory directory;
    const auto original = directory.path() / "original.wav";
    const auto relocated = directory.path() / "moved" / "voice.wav";
    writeMono(original, 0.15);
    std::filesystem::create_directories(relocated.parent_path());
    std::filesystem::copy_file(original, relocated);
    const auto asset = inspectedAudio(original, "Samples/voice");
    const auto document = sampleMapDocument(asset);
    auto relocatedAsset = inspectedAudio(relocated, asset.id);
    relocatedAsset.generation = asset.generation;

    talas::project::ProjectEditor editor(document, builtinRegistry());
    requireOk(
        editor.relocateProjectAsset(relocatedAsset),
        "same-hash sample-map asset relink");
    require(
        editor.document().assets.front().generation == asset.generation
            && editor.document().sampleMapLibrary.maps.front().regions.front()
                    .assetId
                == asset.id,
        "Relink should preserve generation and stable sample-region reference");
}

void testMapPlayerReachabilityLoadsWatchesAndPackagesOnce()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "voice.wav";
    writeMono(source, 0.35);
    const auto asset = inspectedAudio(source, "Samples/voice");
    auto document = sampleMapDocument(asset);
    auto layer = document.sampleMapLibrary.maps.front().regions.front();
    layer.regionId = "voiceLayer";
    layer.lowVelocity = 64;
    document.sampleMapLibrary.maps.front().regions.push_back(
        std::move(layer));

    const auto unreachableReferences =
        talas::audio::reachableProjectAssetReferences(
            document,
            builtinRegistry());
    require(
        unreachableReferences.ok()
            && unreachableReferences.references.empty(),
        "an unreferenced sample map should not make its assets runtime-reachable");
    addRawSampleMapPlayer(document);
    const auto resolvedReferences =
        talas::audio::reachableProjectAssetReferences(
            document,
            builtinRegistry());
    require(
        resolvedReferences.ok(),
        "sample-map reachability should resolve without an authority failure");
    const auto& references = resolvedReferences.references;
    require(
        references.size() == 2
            && references[0].assetId == asset.id
            && references[1].assetId == asset.id
            && references[0].parameterId == "sampleMapId"
            && references[0].structuralSourcePath.find("voiceMap")
                != std::string::npos
            && references[0].structuralSourcePath.find("voiceRegion")
                != std::string::npos
            && references[1].structuralSourcePath.find("voiceLayer")
                != std::string::npos,
        "sample-map player reachability should retain every node/map/region structural identity");

    const auto loaded =
        talas::audio::loadReferencedProjectResourceSnapshots(
            document,
            builtinRegistry(),
            std::nullopt);
    require(
        loaded.ok()
            && loaded.snapshots.size() == 1
            && loaded.snapshots.front()->id == asset.id,
        "map-only reachable audio should prepare one immutable resource snapshot");
    const auto watch =
        talas::audio::captureProjectAssetWatchSnapshot(
            document,
            builtinRegistry(),
            std::nullopt);
    require(
        watch.entries.size() == 1
            && watch.entries.front().assetId == asset.id,
        "map-only reachable audio should enter one watcher baseline");

    const auto package = directory.path() / "MapOnly.talaspkg";
    talas::audio::ProjectPackageOptions options;
    options.outputPackagePath = package;
    const auto packaged = talas::audio::saveProjectPackage(document, options);
    require(
        packaged.ok()
            && packaged.assetCount == 1
            && packaged.copiedFileCount == 1,
        "map-only reachable audio should be packaged exactly once");
    const auto resolved = talas::project::resolveProjectPackagePath(package);
    const auto reopened = talas::project::loadProjectFile(
        resolved.projectFilePath);
    require(
        reopened.ok()
            && reopened.document->assets.size() == 1
            && reopened.document->sampleMapLibrary.maps.front().regions.size()
                == 2
            && reopened.document->sampleMapLibrary.maps.front().regions[0]
                    .assetId
                == reopened.document->assets.front().id
            && reopened.document->sampleMapLibrary.maps.front().regions[1]
                    .assetId
                == reopened.document->assets.front().id
            && talas::project::validateProjectPackageAssets(
                   *reopened.document,
                   resolved)
                   .ok(),
        "packaged map-only asset should reopen and verify through canonical identities");
}
}

int main()
{
    testDispositionCandidatesPreserveIdentityAndUndo();
    testReferenceRequiresExplicitExactBytes();
    testEmbedDispositionCancellationCleansOwnedStaging();
    testEmbedDispositionRetainsFailedImportCleanupForRetry();
    testRelinkPreservesSampleRegionAndGeneration();
    testMapPlayerReachabilityLoadsWatchesAndPackagesOnce();
    return 0;
}
