#include "talas/audio/DiskCacheCapture.h"

#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectJson.h"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

static_assert(noexcept(
    std::declval<talas::audio::PreparedDiskCacheCapture&>().publishFile(
        std::declval<std::string&>())));

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

struct TempDirectory
{
    std::filesystem::path path;

    TempDirectory()
    {
        path = std::filesystem::temp_directory_path()
            / ("talas-disk-cache-capture-"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count()));
        std::filesystem::create_directories(path);
    }

    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

talas::project::ProjectDocument captureDocument()
{
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.002, 0.006};
    document.graph.nodes = {
        {"osc",
         "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}},
         {}},
        {"cache",
         "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/Cache"}},
          {"record", {}, {}, true}},
         {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument nestedCaptureDocument()
{
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.002, 0.006};
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "cachedVoice";
    definition.displayName = "Cached Voice";
    definition.graph.nodes = {
        {"osc",
         "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}},
         {}},
        {"cache",
         "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/Cache"}},
          {"record", {}, {}, true}},
         {}},
    };
    definition.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
    };
    definition.outputs = {
        {"audioOut", talas::core::PortType::Audio, "cache", "audioOut"},
    };
    document.soundDefinitions.push_back(std::move(definition));
    document.graph.nodes = {
        {"voice", {}, {}, std::string{"cachedVoice"}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"voice", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::audio::DiskCacheCaptureOptions optionsFor(
    const talas::project::ProjectEditor& editor,
    const std::filesystem::path& projectPath)
{
    talas::audio::DiskCacheCaptureOptions options;
    options.projectFilePath = projectPath;
    options.sourcePath = "cache";
    options.assetId = "Audio/Cache";
    options.relativeAssetPath = "DiskCache/cache-g1.wav";
    options.assetGeneration = 1;
    options.expectedDocumentAuthorityGeneration =
        editor.documentAuthorityGeneration();
    options.expectedCanonicalDocumentBytes =
        talas::project::serializeProjectDocument(editor.document());
    options.expectedDeviceGeneration = 9;
    options.sampleRate = 8000.0;
    options.maximumBlockFrames = 7;
    return options;
}

void testCaptureWritesExactLoopAndDefersCrossFileCommit()
{
    TempDirectory directory;
    const auto projectPath = directory.path / "capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{captureDocument(), registry};
    auto result = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, optionsFor(editor, projectPath));
    require(
        result.ok() && result.renderedFrames == 48,
        "capture worker should prepare the exact loop through its end frame");
    require(
        !std::filesystem::exists(result.prepared->destination())
            && std::filesystem::is_regular_file(
                result.prepared->temporaryPath()),
        "worker must retain verified bytes in private staging until admission");

    const auto wave = talas::audio::readPcm16Wav(
        result.prepared->temporaryPath());
    require(
        wave.ok() && wave.sampleRate == 8000
            && wave.channels.size() == 1
            && wave.channels.front().size() == 32,
        "staged capture should be exact mono PCM16 over the loop range");
    for (std::size_t frame = 0; frame < wave.channels.front().size(); ++frame)
    {
        const auto expected = std::sin(
            2.0 * std::numbers::pi
            * 1000.0 * static_cast<double>(frame + 16U) / 8000.0);
        require(
            std::abs(wave.channels.front()[frame] - expected)
                <= 1.0 / 32767.0 + 1.0e-9,
            "captured sample should match the upstream frame-zero render oracle");
    }

    talas::project::PreparedDiskCacheCaptureEdit preparedEdit;
    require(
        editor.prepareDiskCacheCapture(
                  result.prepared->editorPublication(), preparedEdit)
            .ok(),
        "verified staged bytes should prepare the complete editor candidate");
    std::string publicationError;
    require(
        editor.diskCacheCaptureStillCurrent(preparedEdit)
            && result.prepared->expectedDeviceGeneration() == 9
            && result.prepared->publishFile(publicationError)
            && editor.commitPreparedDiskCacheCapture(
                std::move(preparedEdit)),
        "file publication followed by prepared editor commit should succeed without a fallible gap");
    require(
        std::filesystem::is_regular_file(
            directory.path / "DiskCache/cache-g1.wav")
            && editor.document().assets.size() == 1
            && talas::project::diskCacheAssetAuthority(
                   editor.document().assets.front())
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
        "successful admission should publish one exact captured asset authority");
}

void testCancellationAndConcurrentDestinationRemainFailureAtomic()
{
    TempDirectory directory;
    const auto projectPath = directory.path / "capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{captureDocument(), registry};
    auto options = optionsFor(editor, projectPath);
    options.shouldCancel = [] { return true; };
    auto cancelled = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, std::move(options));
    require(
        cancelled.cancelled()
            && !std::filesystem::exists(directory.path / "DiskCache/cache-g1.wav")
            && !editor.canUndo(),
        "cancelled capture must not publish bytes or mutate project history");

    bool renderedOneBlock = false;
    options = optionsFor(editor, projectPath);
    options.reportProgress = [&](double) { renderedOneBlock = true; };
    options.shouldCancel = [&] { return renderedOneBlock; };
    auto midRender = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, std::move(options));
    require(
        midRender.cancelled()
            && !std::filesystem::exists(
                directory.path / "DiskCache/cache-g1.wav")
            && std::ranges::none_of(
                std::filesystem::directory_iterator{
                    directory.path / "DiskCache"},
                [](const auto&) { return true; }),
        "mid-render cancellation must remove the partial private staging tree");

    std::filesystem::create_directories(directory.path / "DiskCache");
    const auto destination = directory.path / "DiskCache/cache-g1.wav";
    std::ofstream(destination) << "sentinel";
    auto collision = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, optionsFor(editor, projectPath));
    std::ifstream sentinel(destination);
    std::string bytes;
    sentinel >> bytes;
    require(
        !collision.ok() && bytes == "sentinel" && !editor.canUndo(),
        "existing/concurrent destination must remain byte-exact and uncommitted");
}

void testNestedCaptureUsesExactExpandedStructuralIdentity()
{
    TempDirectory directory;
    const auto projectPath = directory.path / "capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{nestedCaptureDocument(), registry};
    auto options = optionsFor(editor, projectPath);
    options.sourcePath = "voice/cachedVoice::cache";
    auto result = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, std::move(options));
    require(
        result.ok()
            && result.prepared->editorPublication().sourcePath
                == "voice/cachedVoice::cache",
        "nested capture must retain exact expanded structural identity");
    const auto wave = talas::audio::readPcm16Wav(
        result.prepared->temporaryPath());
    require(
        wave.ok() && wave.channels.size() == 1
            && wave.channels.front().size() == 32,
        "nested capture should render the exact definition-owned source range");
}

void testCaptureIgnoresAssetsOutsideExactUpstreamClosure()
{
    TempDirectory directory;
    const auto projectPath = directory.path / "capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = captureDocument();
    talas::project::ProjectAsset unrelated;
    unrelated.id = "Audio/Unrelated";
    unrelated.kind = talas::project::ProjectAssetKind::Audio;
    unrelated.path = "Audio/missing-unrelated.wav";
    unrelated.disposition =
        talas::project::ProjectAssetDisposition::Referenced;
    unrelated.contentHash.assign(64, 'a');
    unrelated.channelCount = 1;
    unrelated.sampleRate = 8000.0;
    unrelated.frameCount = 8;
    unrelated.encoding = "pcm16-wav";
    unrelated.generation = 1;
    unrelated.provenance = "repository-original";
    document.assets.push_back(std::move(unrelated));
    talas::project::ProjectEditor editor{document, registry};
    auto result = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, optionsFor(editor, projectPath));
    require(
        result.ok(),
        "an unavailable asset outside the exact upstream closure must not block capture");
}

void testCaptureReusesRegistryOnBoundedWorkerStack()
{
    TempDirectory directory;
    const auto projectPath = directory.path / "capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{captureDocument(), registry};
    auto options = optionsFor(editor, projectPath);
    bool captured = false;
    std::string error;
    std::thread worker([&] {
        auto result = talas::audio::prepareProjectDiskCacheCapture(
            editor.document(), registry, std::move(options));
        captured = result.ok();
        error = std::move(result.error);
    });
    worker.join();
    require(
        captured,
        error.empty()
            ? "DiskCache capture should reuse registry authority on a bounded worker stack"
            : error);
}

void testCaptureRejectsSymlinkedAncestorBeforeFilesystemMutation()
{
    TempDirectory directory;
    const auto directRoot = directory.path / "direct";
    const auto directProjectDirectory = directRoot / "project";
    std::filesystem::create_directories(directProjectDirectory);
    const auto indirectRoot = directory.path / "indirect";
    std::error_code symlinkError;
    std::filesystem::create_directory_symlink(
        directRoot, indirectRoot, symlinkError);
    if (symlinkError)
        return;

    const auto projectPath = indirectRoot / "project/capture.talas.json";
    std::ofstream(projectPath) << "saved-project";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{captureDocument(), registry};
    auto result = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, optionsFor(editor, projectPath));
    require(
        !result.ok()
            && !std::filesystem::exists(
                directProjectDirectory / "DiskCache"),
        "capture must reject a symlinked ancestor before creating project-local artifacts");
}
}

int main()
{
    testCaptureWritesExactLoopAndDefersCrossFileCommit();
    testCancellationAndConcurrentDestinationRemainFailureAtomic();
    testNestedCaptureUsesExactExpandedStructuralIdentity();
    testCaptureIgnoresAssetsOutsideExactUpstreamClosure();
    testCaptureReusesRegistryOnBoundedWorkerStack();
    testCaptureRejectsSymlinkedAncestorBeforeFilesystemMutation();
    return 0;
}
