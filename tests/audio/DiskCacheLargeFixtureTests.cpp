#include "talas/audio/DiskCacheCapture.h"

#include "talas/audio/ProjectAssetLoader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace
{
using Clock = std::chrono::steady_clock;

constexpr double sampleRate = 8000.0;
constexpr std::size_t captureBlockFrames = 4096;
constexpr std::size_t callbackBlockFrames = 256;
constexpr std::uint64_t captureBudgetNs = 30'000'000'000ULL;
constexpr std::uint64_t preparationBudgetNs = 5'000'000'000ULL;
constexpr std::uint64_t callbackP99BudgetNs = 5'333'334ULL;

#if defined(NDEBUG)
constexpr bool optimizedBuild = true;
constexpr std::size_t callbackSamples = 128;
#else
constexpr bool optimizedBuild = false;
constexpr std::size_t callbackSamples = 16;
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool sanitizerInstrumented = true;
#else
constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool sanitizerInstrumented = true;
#else
constexpr bool sanitizerInstrumented = false;
#endif

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::uint64_t elapsedNs(Clock::time_point started)
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - started)
            .count());
}

std::uint64_t nearestRank(
    std::vector<std::uint64_t> values,
    std::size_t numerator,
    std::size_t denominator)
{
    std::ranges::sort(values);
    const auto rank = (values.size() * numerator + denominator - 1U)
        / denominator;
    return values[std::max<std::size_t>(1, rank) - 1U];
}

struct TempDirectory
{
    std::filesystem::path path;

    TempDirectory()
    {
        path = std::filesystem::temp_directory_path()
            / ("talas-disk-cache-large-"
               + std::to_string(
                   Clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }

    ~TempDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

talas::project::ProjectDocument maximumCaptureDocument()
{
    talas::project::ProjectDocument document;
    document.timeline.loop = {
        true,
        0.0,
        static_cast<double>(
            talas::project::maximumDiskCacheCaptureFrames)
            / sampleRate,
    };
    document.graph.nodes = {
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"997 hz"}},
          {"amplitude", std::string{"0.5"}}}, {}},
        {"cache", "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/MaximumCache"}},
          {"record", {}, {}, true}}, {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    return document;
}

void testMaximumCapturePreparationAndCallbackBudgets()
{
    TempDirectory directory;
    const auto projectFile = directory.path / "maximum.talas.json";
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{maximumCaptureDocument(), registry};
    {
        std::ofstream project{projectFile, std::ios::binary};
        project << talas::project::serializeProjectDocument(editor.document());
    }

    talas::audio::DiskCacheCaptureOptions options;
    options.projectFilePath = projectFile;
    options.sourcePath = "cache";
    options.assetId = "Audio/MaximumCache";
    options.relativeAssetPath = "DiskCache/maximum-cache-g1.wav";
    options.assetGeneration = 1;
    options.expectedDocumentAuthorityGeneration =
        editor.documentAuthorityGeneration();
    options.expectedCanonicalDocumentBytes =
        talas::project::serializeProjectDocument(editor.document());
    options.expectedDeviceGeneration = 1;
    options.sampleRate = sampleRate;
    options.maximumBlockFrames = captureBlockFrames;

    const auto captureStarted = Clock::now();
    auto captured = talas::audio::prepareProjectDiskCacheCapture(
        editor.document(), registry, std::move(options));
    const auto captureNs = elapsedNs(captureStarted);
    require(captured.ok(),
        "maximum DiskCache capture failed: " + captured.error);
    const auto& publication = captured.prepared->editorPublication();
    require(
        captured.renderedFrames
                == talas::project::maximumDiskCacheCaptureFrames
            && publication.asset.frameCount
                == talas::project::maximumDiskCacheCaptureFrames
            && publication.asset.sampleRate == sampleRate
            && publication.asset.channelCount == 1
            && publication.asset.encoding == "pcm16-wav"
            && talas::project::diskCacheAssetAuthority(publication.asset)
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
        "maximum capture must retain exact verified media/provenance authority");
    std::error_code sizeError;
    const auto stagedBytes = std::filesystem::file_size(
        captured.prepared->temporaryPath(), sizeError);
    require(
        !sizeError
            && stagedBytes
                == 44ULL
                    + 2ULL
                        * talas::project::maximumDiskCacheCaptureFrames,
        "maximum capture must write one exact bounded mono PCM16-WAV");

    talas::project::PreparedDiskCacheCaptureEdit preparedEdit;
    require(
        editor.prepareDiskCacheCapture(
                  publication, preparedEdit)
            .ok(),
        "maximum capture must prepare one exact editor publication");
    std::string publicationError;
    require(
        captured.prepared->publishFile(publicationError)
            && editor.commitPreparedDiskCacheCapture(
                std::move(preparedEdit)),
        "maximum capture must publish file and document atomically: "
            + publicationError);

    const auto preparationStarted = Clock::now();
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        editor.document(), registry, projectFile);
    require(loaded.ok(),
        "maximum captured resource must load as immutable authority");
    talas::runtime::RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = callbackBlockFrames;
    settings.resourceSnapshots = std::move(loaded.snapshots);
    auto compiled = talas::runtime::compileProject(
        editor.document(), registry, std::move(settings));
    require(compiled.ok(),
        compiled.diagnostics.empty()
            ? "maximum captured playback must compile"
            : compiled.diagnostics.front().message);
    talas::runtime::OfflineRenderSession offline{*compiled.graph};
    const auto offlineBlock = offline.render(callbackBlockFrames);
    require(
        offlineBlock.ok()
            && offlineBlock.mono.size() == callbackBlockFrames
            && std::ranges::any_of(offlineBlock.mono, [](double value) {
                return std::abs(value) > 1.0e-9;
            }),
        "maximum captured asset must remain audibly renderable offline");
    talas::runtime::RealtimeRenderSession session{*compiled.graph};
    require(session.ready(), std::string{session.error()});
    const auto preparationNs = elapsedNs(preparationStarted);

    const auto streamDeadline = Clock::now() + std::chrono::seconds{2};
    while (Clock::now() < streamDeadline)
    {
        const auto health = session.streamHealth();
        if (!health.consumers.empty()
            && health.consumers.front().health.bufferedBlocks > 0)
        {
            break;
        }
        std::this_thread::yield();
    }
    const auto initialHealth = session.streamHealth();
    require(
        !initialHealth.consumers.empty()
            && initialHealth.consumers.front().health.bufferedBlocks > 0,
        "maximum captured playback worker must publish a prepared block");

    std::array<double, callbackBlockFrames> output{};
    for (std::size_t warmup = 0; warmup < 2; ++warmup)
    {
        require(session.render(output),
            "maximum captured playback warmup must render");
    }
    std::vector<std::uint64_t> callbackDurations;
    callbackDurations.reserve(callbackSamples);
    double checksum = 0.0;
    for (std::size_t sample = 0; sample < callbackSamples; ++sample)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{32});
        output.fill(0.0);
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_release);
        const auto started = Clock::now();
        const auto rendered = session.render(output);
        const auto duration = elapsedNs(started);
        countAllocations.store(false, std::memory_order_release);
        require(rendered,
            "maximum captured playback callback must render");
        require(
            allocationCount.load(std::memory_order_relaxed) == 0,
            "maximum captured playback callback must not allocate");
        require(std::ranges::all_of(output, [](double value) {
                    return std::isfinite(value);
                }),
            "maximum captured playback callback output must remain finite");
        checksum += std::abs(output[(sample * 17U) % output.size()]);
        callbackDurations.push_back(duration);
    }
    const auto callbackP99 = nearestRank(
        callbackDurations, 99, 100);
    const auto health = session.streamHealth();
    require(
        health.consumerCount == 1
            && health.consumers.size() == 1
            && health.consumers.front().role
                == talas::runtime::RealtimeStreamRole::Playback
            && health.consumers.front().resourceId
                == "Audio/MaximumCache",
        "maximum callback fixture must expose one exact playback consumer");
    require(
        checksum > 1.0e-9,
        "maximum callback fixture must prove audible exact cache playback; checksum="
            + std::to_string(checksum)
            + " missingFrames="
            + std::to_string(health.totals.missingFrames)
            + " underruns="
            + std::to_string(health.totals.underrunEvents));
    require(
        !optimizedBuild || sanitizerInstrumented
            || (captureNs <= captureBudgetNs
                && preparationNs <= preparationBudgetNs
                && callbackP99 <= callbackP99BudgetNs),
        "optimized maximum DiskCache fixture exceeded its fixed budgets");
    std::cout << "DISK_CACHE_MAX_FIXTURE capture_ns=" << captureNs
              << " preparation_ns=" << preparationNs
              << " callback_p99_ns=" << callbackP99
              << " staged_bytes=" << stagedBytes
              << " checksum=" << checksum << '\n';
}
}

int main()
{
    testMaximumCapturePreparationAndCallbackBudgets();
}
