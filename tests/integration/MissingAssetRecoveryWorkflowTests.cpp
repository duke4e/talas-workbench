#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/ProjectPackager.h"
#include "talas/cli/Cli.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectAssetResolver.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectRecoveryFile.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
              / ("talas-missing-asset-workflow-"
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

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "file should be readable: " + path.string());
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
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

void requireCliOk(
    CliResult result,
    std::string_view operation)
{
    require(
        result.exitCode == 0 && result.error.empty(),
        std::string{operation} + " failed: " + result.error);
}

talas::project::ProjectParameterExpression& parameter(
    talas::project::ProjectDocument& document,
    std::string_view nodeId,
    std::string_view parameterId)
{
    const auto node = std::ranges::find(
        document.graph.nodes,
        nodeId,
        &talas::project::ProjectNode::instanceId);
    require(node != document.graph.nodes.end(), "fixture node is missing");
    const auto found = std::ranges::find(
        node->parameters,
        parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    require(found != node->parameters.end(), "fixture parameter is missing");
    return *found;
}

[[nodiscard]] bool renderBecomesAudible(
    talas::runtime::ProjectRuntimeBridge& bridge)
{
    std::array<float, 128> output{};
    std::array<float*, 1> channels{output.data()};
    for (int attempt = 0; attempt < 80; ++attempt)
    {
        output.fill(0.0F);
        if (!bridge.render(
                channels.data(),
                channels.size(),
                output.size()))
        {
            return false;
        }
        if (std::ranges::any_of(output, [](float sample) {
                return std::isfinite(sample)
                    && std::abs(sample) > 1.0e-6F;
            }))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

talas::runtime::RenderSettings settingsWithAssets(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::filesystem::path& projectPath)
{
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry,
        projectPath);
    require(
        loaded.ok() && loaded.snapshots.size() == 2,
        loaded.failure.has_value()
            ? "resource preparation failed: " + loaded.failure->message
            : "resource preparation did not produce two snapshots");
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8192.0;
    settings.maxBlockFrames = 128;
    settings.resourceSnapshots = std::move(loaded.snapshots);
    return settings;
}

void testMovedM3AssetRecoveryWorkflow()
{
    TemporaryDirectory directory;
    const auto sourceExample =
        std::filesystem::path{TALAS_SOURCE_DIR}
        / "examples/m3_streaming_health.talas.json";
    const auto sourceWav =
        std::filesystem::path{TALAS_SOURCE_DIR}
        / "examples/resources/m3-stream-source.wav";
    const auto loadedExample = talas::project::loadProjectFile(sourceExample);
    require(loadedExample.ok(), "M3 streaming example should load");
    auto document = std::move(*loadedExample.document);
    require(
        document.assets.size() == 1,
        "M3 streaming fixture should start with one accepted Audio identity");

    const auto sourceProject =
        directory.path() / "source/MovedStreaming.talas.json";
    auto playerAsset = document.assets.front();
    playerAsset.path = "resources/player-source.wav";
    auto cacheAsset = playerAsset;
    cacheAsset.id = "Audio/M3StreamCache";
    cacheAsset.path = "resources/cache-source.wav";
    cacheAsset.provenance =
        "project-original M3 streaming cache alias";
    document.assets = {playerAsset, cacheAsset};
    parameter(document, "cache", "fileName").resource = cacheAsset.id;
    const auto playerSource =
        sourceProject.parent_path()
        / talas::project::pathFromUtf8(playerAsset.path);
    const auto cacheSource =
        sourceProject.parent_path()
        / talas::project::pathFromUtf8(cacheAsset.path);
    copyFile(sourceWav, playerSource);
    copyFile(sourceWav, cacheSource);
    require(
        talas::project::saveProjectFile(sourceProject, document).ok(),
        "two-identity M3 source project should save");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(document, registry);
    const auto firstStage = bridge.compileAndStage(
        settingsWithAssets(document, registry, sourceProject));
    require(firstStage.ok(), "source project should stage: " + firstStage.message);
    bridge.setPlaying(true);
    require(
        renderBecomesAudible(bridge),
        "source M3 streaming generation should become audible");

    const auto acceptedPlayer = bridge.acceptedResourceIdentity(
        playerAsset.id);
    const auto acceptedCache = bridge.acceptedResourceIdentity(
        cacheAsset.id);
    require(
        acceptedPlayer.has_value()
            && acceptedCache.has_value()
            && acceptedPlayer->generation == playerAsset.generation
            && acceptedCache->generation == cacheAsset.generation
            && acceptedPlayer->contentHash == playerAsset.contentHash
            && acceptedCache->contentHash == cacheAsset.contentHash,
        "activated source graph should publish both accepted identities");

    const auto relocatedWav =
        directory.path() / "library/relocated/m3-stream-source.wav";
    copyFile(sourceWav, relocatedWav);
    std::error_code ignored;
    std::filesystem::remove(playerSource, ignored);
    ignored.clear();
    std::filesystem::remove(cacheSource, ignored);

    const auto search = talas::project::searchProjectAssetDirectory(
        bridge.editor().document().assets,
        relocatedWav.parent_path(),
        sourceProject);
    require(
        search.ok()
            && search.hashedFiles == 1
            && search.matches.size() == 2
            && std::ranges::all_of(search.matches, [](const auto& match) {
                return match.candidates.size() == 1
                    && match.candidates.front().mode
                        == talas::project::ProjectAssetResolutionMode::Relocation;
            }),
        "one moved project-original WAV should resolve both accepted identities by exact hash");

    std::vector<talas::project::ProjectAssetResolution> batch;
    for (const auto& match : search.matches)
        batch.push_back(match.candidates.front());
    require(
        bridge.editor().applyProjectAssetResolutions(batch).ok(),
        "exact moved identities should apply as one project transaction");
    const auto relocatedDocument = bridge.editor().document();
    std::ostringstream relocationEvidence;
    for (const auto& asset : relocatedDocument.assets)
    {
        relocationEvidence << asset.id << " actual=" << asset.path
                           << " expected="
                           << talas::project::projectAssetStoredPath(
                                  relocatedWav,
                                  sourceProject)
                           << '\n';
    }
    require(
        bridge.editor().canUndo()
            && relocatedDocument.assets.size() == 2
            && std::ranges::all_of(
                relocatedDocument.assets,
                [&](const auto& asset) {
                    const auto before = asset.id == playerAsset.id
                        ? playerAsset
                        : cacheAsset;
                    auto expected = before;
                    expected.path = talas::project::projectAssetStoredPath(
                        relocatedWav,
                        sourceProject);
                    return asset == expected;
                }),
        "batch relocation should change only both paths and preserve every accepted identity field\n"
            + relocationEvidence.str());
    require(
        renderBecomesAudible(bridge),
        "unstaged relocation edit should leave generation N audible on the active graph");
    require(
        bridge.acceptedResourceIdentity(playerAsset.id)->contentHash
                == playerAsset.contentHash
            && bridge.acceptedResourceIdentity(cacheAsset.id)->contentHash
                == cacheAsset.contentHash,
        "resolver/editor work should not rewrite active runtime identity history");

    require(
        bridge.editor().undo().ok()
            && bridge.editor().document().assets == document.assets
            && !bridge.editor().canUndo(),
        "one Undo should restore the complete pre-relocation manifest batch");
    require(
        bridge.editor().redo().ok()
            && bridge.editor().document().assets
                == relocatedDocument.assets,
        "one Redo should restore the complete exact relocation batch");

    const auto secondStage = bridge.compileAndStage(
        settingsWithAssets(
            bridge.editor().document(),
            registry,
            sourceProject));
    require(
        secondStage.ok() && renderBecomesAudible(bridge),
        "explicit restaging should adopt the relocated paths and remain audible");

    const auto saveAsProject =
        directory.path() / "saved/deeper/RecoveredStreaming.talas.json";
    require(
        bridge.editor().rebaseAssetPathsForProjectMove(
            sourceProject,
            saveAsProject)
            .ok(),
        "Save As should rebase both resolved paths");
    require(
        talas::project::saveProjectFile(
            saveAsProject,
            bridge.editor().document())
            .ok(),
        "rebased resolved project should save");
    const auto reopened = talas::project::loadProjectFile(saveAsProject);
    require(reopened.ok(), "resolved Save As project should reopen");
    require(
        reopened.document->assets.size() == 2
            && std::ranges::all_of(
                reopened.document->assets,
                [&](const auto& asset) {
                    return talas::project::resolveProjectAssetPath(
                               asset,
                               saveAsProject)
                        == relocatedWav.lexically_normal();
                }),
        "Save As/reopen should preserve both resolved native targets");

    talas::project::ProjectRecoveryRecord recovery;
    recovery.projectPath = saveAsProject;
    recovery.cleanDocument = document;
    require(
        talas::project::appendProjectRecoveryVersion(
            recovery,
            *reopened.document,
            1000),
        "resolved transaction should append as one recovery version");
    const auto recoveryPath =
        directory.path() / "recovery/current.talas-recovery.json";
    require(
        talas::project::saveProjectRecoveryFile(
            recoveryPath,
            recovery)
            .ok(),
        "resolved recovery record should save");
    const auto loadedRecovery =
        talas::project::loadProjectRecoveryFile(recoveryPath);
    require(
        loadedRecovery.ok()
            && talas::project::serializeProjectDocument(
                   loadedRecovery.record->versions.back()
                       .recoveredDocument)
                == talas::project::serializeProjectDocument(
                    *reopened.document),
        "recovery should retain exactly the accepted document transaction");
    const auto recoveryBytes = readText(recoveryPath);
    require(
        recoveryBytes.find("searchActive") == std::string::npos
            && recoveryBytes.find("ignored") == std::string::npos
            && recoveryBytes.find("candidate") == std::string::npos,
        "recovery should not serialize search progress, ignore state, or candidate caches");

    talas::audio::ProjectPackageOptions packageOptions;
    const auto package =
        directory.path() / "portable/RecoveredStreaming.talaspkg";
    packageOptions.outputPackagePath = package;
    packageOptions.sourceProjectFilePath = saveAsProject;
    const auto packaged = talas::audio::saveProjectPackage(
        *reopened.document,
        packageOptions);
    require(
        packaged.ok()
            && packaged.assetCount == 2
            && packaged.copiedFileCount == 1,
        "portable package should deduplicate the shared relocated WAV without merging asset identities");
    const auto movedPackage =
        directory.path() / "moved/RecoveredStreaming.talaspkg";
    std::filesystem::create_directories(movedPackage.parent_path());
    std::filesystem::rename(package, movedPackage);
    const auto resolvedPackage =
        talas::project::resolveProjectPackagePath(movedPackage);
    const auto packagedDocument = resolvedPackage.ok()
        ? talas::project::loadProjectFile(
              resolvedPackage.projectFilePath)
        : talas::project::ProjectFileLoadResult{};
    require(
        resolvedPackage.ok()
            && packagedDocument.ok()
            && talas::project::validateProjectPackageAssets(
                   *packagedDocument.document,
                   resolvedPackage)
                   .ok()
            && std::ranges::all_of(
                packagedDocument.document->assets,
                [](const auto& asset) {
                    return asset.disposition
                        == talas::project::ProjectAssetDisposition::Embedded;
                }),
        "moved package should remain self-contained and never require an external resolver path");

    requireCliOk(
        runCli({"validate", "--project", saveAsProject.string()}),
        "CLI validate after relocation");
    requireCliOk(
        runCli({
            "render",
            "--project",
            saveAsProject.string(),
            "--frames",
            "128",
            "--sample-rate",
            "8192",
            "--max-block-frames",
            "128",
            "--format",
            "json",
        }),
        "CLI render after relocation");
    const auto exportedWav =
        directory.path() / "exports/recovered.wav";
    std::filesystem::create_directories(exportedWav.parent_path());
    requireCliOk(
        runCli({
            "render",
            "--project",
            saveAsProject.string(),
            "--frames",
            "128",
            "--sample-rate",
            "8192",
            "--max-block-frames",
            "128",
            "--output",
            exportedWav.string(),
        }),
        "CLI export after relocation");
    require(
        std::filesystem::is_regular_file(exportedWav),
        "CLI export should publish the recovered WAV destination");
    requireCliOk(
        runCli({"validate", "--project", movedPackage.string()}),
        "CLI validate after package move");

    const auto embeddedPath =
        movedPackage
        / talas::project::pathFromUtf8(
            packagedDocument.document->assets.front().path);
    std::ofstream tampered(
        embeddedPath,
        std::ios::binary | std::ios::trunc);
    tampered << "tampered";
    tampered.close();
    require(
        !talas::project::validateProjectPackageAssets(
             *packagedDocument.document,
             resolvedPackage)
             .ok(),
        "tampered embedded bytes should remain a package-integrity failure instead of external relocation input");

    bridge.clearAfterAudioStopped();
}
}

int main()
{
    try
    {
        testMovedM3AssetRecoveryWorkflow();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
