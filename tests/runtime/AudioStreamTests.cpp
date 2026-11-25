#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/runtime/AudioStream.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <chrono>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <memory>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
thread_local std::size_t checkpointFaultCountdown = std::numeric_limits<std::size_t>::max();
thread_local bool checkpointFaultTriggered = false;

void injectCheckpointAllocationFailure()
{
    if (checkpointFaultCountdown == std::numeric_limits<std::size_t>::max()) return;
    if (checkpointFaultCountdown-- == 0)
    {
        checkpointFaultCountdown = std::numeric_limits<std::size_t>::max();
        checkpointFaultTriggered = true;
        throw std::bad_alloc{};
    }
}
}

void* operator new(std::size_t size)
{
    injectCheckpointAllocationFailure();
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    injectCheckpointAllocationFailure();
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

namespace talas::runtime
{
struct RealtimeRenderSessionTestAccess
{
    static auto checkpoint(RealtimeRenderSession& session) { return session.beginOfflineTransaction(); }
};
struct AudioStreamConsumerTestAccess
{
    static bool beginCallback(AudioStreamConsumer& consumer) { return consumer.beginCallbackCheckpoint(); }
    static void commitCallback(AudioStreamConsumer& consumer) { consumer.commitCallbackCheckpoint(); }
    static void rollbackCallback(AudioStreamConsumer& consumer) { consumer.rollbackCallbackCheckpoint(); }

    static auto checkpoint(AudioStreamConsumer& consumer, bool deferInspection = false, bool deterministicReads = false)
    { return consumer.beginOfflineCheckpoint(deferInspection, deterministicReads); }
};
}

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message)
{
    require(
        std::abs(actual - expected) <= 1.0e-12,
        message + ": expected " + std::to_string(expected)
            + ", received " + std::to_string(actual));
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-audio-stream-tests-"
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

talas::core::ResourceSnapshotPtr writeSnapshot(
    const std::filesystem::path& path,
    double sampleRate,
    const std::vector<std::vector<double>>& channels,
    std::uint64_t generation = 1,
    bool capturedDiskCache = false)
{
    std::string error;
    require(
        talas::audio::writePcm16Wav(
            path,
            sampleRate,
            channels,
            error),
        "test should write PCM16 WAV: " + error);
    const auto hash =
        talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "test WAV should hash");

    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Audio/Test";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = generation;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount =
        static_cast<std::uint32_t>(channels.size());
    snapshot.sampleRate = sampleRate;
    snapshot.frameCount = channels.front().size();
    snapshot.encoding = "pcm16-wav";
    snapshot.projectAssetPath =
        "DiskCache/cache-g" + std::to_string(generation) + ".wav";
    snapshot.provenance = capturedDiskCache
        ? talas::project::makeDiskCacheCaptureProvenance(
              std::string(
                  talas::project::diskCacheCaptureFingerprintBytes,
                  'a'))
        : "repository-original";
    if (capturedDiskCache)
    {
        std::ifstream input(path, std::ios::binary);
        snapshot.audioBytes.assign(
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{});
        require(!snapshot.audioBytes.empty(),
            "captured test snapshot should retain exact WAV bytes");
    }
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

// Sample files of other word sizes, as other programs write them: a RIFF/WAVE
// file of one format, or an AIFF file at 48 kHz.
void appendUnsigned(
    std::vector<std::uint8_t>& bytes,
    std::uint64_t value,
    std::size_t count,
    bool bigEndian = false)
{
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto shift = 8U * (bigEndian ? count - 1U - index : index);
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void appendTag(std::vector<std::uint8_t>& bytes, std::string_view tag)
{
    bytes.insert(bytes.end(), tag.begin(), tag.end());
}

std::vector<std::uint8_t> sampleWave(
    std::uint16_t format,
    std::uint16_t channels,
    std::uint16_t bits,
    const std::vector<std::uint8_t>& data)
{
    const auto blockAlign = static_cast<std::uint16_t>(channels * bits / 8U);
    std::vector<std::uint8_t> bytes;
    appendTag(bytes, "RIFF");
    appendUnsigned(bytes, 36U + data.size() + (data.size() & 1U), 4);
    appendTag(bytes, "WAVE");
    appendTag(bytes, "fmt ");
    appendUnsigned(bytes, 16, 4);
    appendUnsigned(bytes, format, 2);
    appendUnsigned(bytes, channels, 2);
    appendUnsigned(bytes, 48000, 4);
    appendUnsigned(bytes, 48000U * blockAlign, 4);
    appendUnsigned(bytes, blockAlign, 2);
    appendUnsigned(bytes, bits, 2);
    appendTag(bytes, "data");
    appendUnsigned(bytes, data.size(), 4);
    bytes.insert(bytes.end(), data.begin(), data.end());
    if ((data.size() & 1U) != 0U)
        bytes.push_back(0);
    return bytes;
}

std::vector<std::uint8_t> sampleAiff(
    std::uint16_t channels,
    std::uint16_t bits,
    const std::vector<std::uint8_t>& data)
{
    const auto frames = data.size() / (channels * ((bits + 7U) / 8U));
    std::vector<std::uint8_t> bytes;
    appendTag(bytes, "FORM");
    appendUnsigned(bytes, 4U + 26U + 16U + data.size(), 4, true);
    appendTag(bytes, "AIFF");
    appendTag(bytes, "COMM");
    appendUnsigned(bytes, 18, 4, true);
    appendUnsigned(bytes, channels, 2, true);
    appendUnsigned(bytes, frames, 4, true);
    appendUnsigned(bytes, bits, 2, true);
    // 48000 as an 80-bit extended float.
    appendUnsigned(bytes, 0x400EU, 2, true);
    appendUnsigned(bytes, 0xBB80000000000000ULL, 8, true);
    appendTag(bytes, "SSND");
    appendUnsigned(bytes, 8U + data.size(), 4, true);
    appendUnsigned(bytes, 0, 8, true);
    bytes.insert(bytes.end(), data.begin(), data.end());
    return bytes;
}

// A snapshot of `bytes` as an imported sample file.
talas::core::ResourceSnapshotPtr writeSampleFileSnapshot(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes,
    std::uint32_t channelCount,
    std::uint64_t frameCount)
{
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "test sample file should hash");
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Audio/Imported";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = channelCount;
    snapshot.sampleRate = 48000.0;
    snapshot.frameCount = frameCount;
    snapshot.encoding = "sample-file";
    snapshot.projectAssetPath = "media/imported";
    snapshot.provenance = "repository-original";
    return std::make_shared<const talas::core::ResourceSnapshot>(std::move(snapshot));
}

talas::runtime::AudioStreamConfiguration manualConfiguration()
{
    talas::runtime::AudioStreamConfiguration configuration;
    configuration.blockFrames = 2;
    configuration.requestCapacity = 4;
    configuration.responseCapacity = 4;
    configuration.decodedCacheBlocks = 4;
    configuration.lowWatermark = 1;
    configuration.highWatermark = 3;
    configuration.workerMode =
        talas::runtime::AudioStreamWorkerMode::ManualTest;
    return configuration;
}

talas::runtime::AudioStreamPreparedWindowBudget preparedWindowBudget()
{
    return {
        4096,
        64U * 1024U * 1024U,
        0,
        0,
    };
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectParameterExpression booleanParameter(
    std::string id,
    bool value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(value);
    return parameter;
}

// These fixtures hear a stereo file's channels averaged, as projects
// before format 95 do (`channels` `legacyMono`).
talas::project::ProjectParameterExpression averagedChannels()
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = "channels";
    parameter.choice = "legacyMono";
    return parameter;
}

talas::project::ProjectDocument diskPlayerCacheDocument(
    std::string rateScale = "1",
    std::string filePosition = "0 s",
    std::string duration = "1 s",
    bool record = false)
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        {"cache", "talas.diskCache", {
             resourceParameter("fileName", "Audio/Test"),
             booleanParameter("record", record),
         }, {}},
        {"player", "talas.diskPlayer", {
             resourceParameter("fileName", "Audio/Test"),
             expressionParameter(
                 "rateScale",
                 std::move(rateScale)),
             expressionParameter(
                 "filePosition",
                 std::move(filePosition)),
             expressionParameter(
                 "duration",
                 std::move(duration)),
             averagedChannels(),
         }, {}},
    };
    document.graph.connections = {
        {"player", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument capturedDiskCacheDocument(
    const talas::core::ResourceSnapshot& snapshot,
    std::string provenance = talas::project::makeDiskCacheCaptureProvenance(
        std::string(talas::project::diskCacheCaptureFingerprintBytes, 'a')))
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        {"cache", "talas.diskCache", {
             resourceParameter("fileName", snapshot.id),
             booleanParameter("record", false),
         }, {}},
        {"input", "talas.oscillator", {
             expressionParameter("frequency", "1000 hz"),
             expressionParameter("amplitude", "0.75"),
         }, {}},
    };
    document.graph.connections = {
        {"input", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    document.assets.push_back({
        snapshot.id,
        talas::project::ProjectAssetKind::Audio,
        "DiskCache/cache-g" + std::to_string(snapshot.generation)
            + ".wav",
        talas::project::ProjectAssetDisposition::Referenced,
        snapshot.contentHash,
        snapshot.channelCount,
        snapshot.sampleRate,
        snapshot.frameCount,
        snapshot.encoding,
        snapshot.generation,
        std::move(provenance),
    });
    return document;
}

talas::project::ProjectDocument connectedDiskPlayerDocument(
    std::string connectedRate,
    std::string connectedPosition,
    std::string connectedTrigger,
    std::string staticRate = "1",
    std::string staticPosition = "0 s",
    std::string staticTrigger = "1")
{
    auto document = diskPlayerCacheDocument(
        std::move(staticRate),
        std::move(staticPosition),
        "1 s");
    document.graph.nodes.push_back({
        "rateControl",
        "talas.constant",
        {expressionParameter(
            "value",
            std::move(connectedRate))},
        {},
    });
    document.graph.nodes.push_back({
        "positionControl",
        "talas.constant",
        {expressionParameter(
            "value",
            std::move(connectedPosition))},
        {},
    });
    document.graph.nodes.push_back({
        "triggerControl",
        "talas.constant",
        {expressionParameter(
            "value",
            std::move(connectedTrigger))},
        {},
    });
    auto& player = document.graph.nodes[2];
    for (auto& parameter : player.parameters)
    {
        if (parameter.parameterId == "trigger")
            parameter.expression = std::move(staticTrigger);
    }
    document.graph.connections.push_back({
        "rateControl", "value", "player", "rateScale"});
    document.graph.connections.push_back({
        "positionControl", "value", "player", "filePosition"});
    document.graph.connections.push_back({
        "triggerControl", "value", "player", "trigger"});
    return document;
}

talas::runtime::CompileResult compileAudioDocument(
    talas::core::ResourceSnapshotPtr snapshot,
    double destinationSampleRate,
    talas::project::ProjectDocument document =
        diskPlayerCacheDocument())
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = destinationSampleRate;
    settings.maxBlockFrames = 2;
    settings.resourceSnapshots.push_back(
        std::move(snapshot));
    return talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
}

void requireSameHealth(const talas::runtime::AudioStreamHealthSnapshot& actual,
    const talas::runtime::AudioStreamHealthSnapshot& expected)
{
    require(actual.underrunEvents == expected.underrunEvents && actual.missingFrames == expected.missingFrames
        && actual.discontinuities == expected.discontinuities && actual.requestRejections == expected.requestRejections
        && actual.responseQueueFull == expected.responseQueueFull && actual.staleResponseBlocks == expected.staleResponseBlocks
        && actual.decodedBlocks == expected.decodedBlocks && actual.decodeFailures == expected.decodeFailures
        && actual.endOfStreamFrames == expected.endOfStreamFrames && actual.bufferedBlocks == expected.bufferedBlocks
        && actual.bufferCapacity == expected.bufferCapacity && actual.lowWatermark == expected.lowWatermark
        && actual.highWatermark == expected.highWatermark && actual.pendingRequests == expected.pendingRequests
        && actual.workerState == expected.workerState, "stream checkpoint must restore all health fields");
}

talas::runtime::PreparedRuntimeGraph compileWholeCallStreamFixture(const talas::core::ResourceSnapshotPtr& snapshot)
{
    using namespace talas::runtime;
    auto document = diskPlayerCacheDocument("1", "0 s", "4 s");
    document.graph.nodes.push_back({"gain", "talas.vca", {}, {}});
    document.graph.nodes.push_back({"control", "talas.constant", {expressionParameter("value", "!Gain / !Divisor")}, {}});
    document.graph.connections.back() = {"cache", "audioOut", "gain", "audioIn"};
    document.graph.connections.push_back({"control", "value", "gain", "gain"});
    document.graph.connections.push_back({"gain", "audioOut", "out", "audioIn"});
    RenderSettings settings{64.0, 17};
    settings.resourceSnapshots = {snapshot};
    settings.eventValues = {{"Gain", 1.0}, {"Divisor", 1.0}};
    settings.enableRealtimeAnalysisCapture = true;
    settings.realtimeAnalysisQueueCapacity = 8;
    settings.realtimeAnalysisTargets.push_back({{}, {}, "gain", "audioOut", RealtimeAnalysisKind::Oscilloscope,
        RealtimeAnalysisPayloadKind::AudioBlock, 1, "stream-rollback"});
    auto compiled = compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "whole-call streaming fixture must compile: "
        + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    return std::move(*compiled.graph);
}

void testOfflineWholeCallStreamRollback()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    std::vector<double> source(256);
    for (std::size_t index = 0; index != source.size(); ++index) source[index] = std::sin(index * 0.37) * 0.8;
    const auto snapshot = writeSnapshot(directory.path() / "whole-call.wav", 64.0, {source});
    const auto graph = compileWholeCallStreamFixture(snapshot);
    const auto referenceGraph = compileWholeCallStreamFixture(snapshot);
    const auto oracleOwner = prepareAudioStream(snapshot, manualConfiguration());
    require(oracleOwner.ok(), "whole-call oracle owner must prepare");
    const auto oracle = oracleOwner.owner->loadAllForOffline();
    require(oracle.ok(), "whole-call streaming oracle must load immutable source");
    std::array<OfflineMidiVoiceEvent, 2> events{};
    events[0].kind = events[1].kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    events[0].frame = 57; events[0].batchId = events[0].sequence = 1;
    events[0].eventValueName = "Gain"; events[0].value = 0.5;
    events[1].frame = 121; events[1].batchId = events[1].sequence = 2;
    events[1].eventValueName = "Divisor"; events[1].value = 0.0;
    OfflineRenderSession candidate(graph), reference(referenceGraph);
    require(candidate.scheduleMidiVoiceEvents(events) && reference.scheduleMidiVoiceEvents(events),
        "whole-call streamed Event Value input must be admitted");
    require(candidate.render(41).ok() && reference.render(41).ok(), "stream must commit a nonzero prefix");
    for (int attempt = 0; attempt != 2; ++attempt)
    {
        const auto failed = candidate.render(90);
        require(!failed.ok() && failed.mono.empty() && failed.channels.empty(), "late stream failure must return no partial audio");
    }
    std::array<double, 17> values{}, referenceValues{};
    const auto previous = graph.analysisCapture()->copyLatestRecordResult(0, values);
    const auto referencePrevious = referenceGraph.analysisCapture()->copyLatestRecordResult(0, referenceValues);
    require(previous.status == RealtimeAnalysisCopyStatus::Accepted && previous.header == referencePrevious.header
        && values == referenceValues, "failed streamed call must preserve committed analysis evidence");
    const auto retry = candidate.render(80);
    const auto expected = reference.render(80);
    require(retry.ok() && expected.ok() && retry.mono == expected.mono && retry.mono.size() == 80,
        "streaming retry must restore consumer cursor, queued input and DSP state");
    for (std::size_t index = 0; index != retry.mono.size(); ++index)
        requireNear(retry.mono[index], oracle.data->monoSamples[41 + index] * (41 + index < 57 ? 1.0 : 0.5),
            "whole-call streaming retry must match independent source position and gain");
    require(candidate.render(0).ok() && !candidate.render(1).ok(), "failed stream event must remain at its original boundary");
}

void testOfflineWholeCallStreamAllocationFailureIsAtomic()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    std::vector<double> source(256);
    for (std::size_t index = 0; index != source.size(); ++index) source[index] = std::sin(index * 0.37) * 0.8;
    const auto snapshot = writeSnapshot(directory.path() / "whole-call-allocation.wav", 64.0, {source});
    OfflineMidiVoiceEvent event{};
    event.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
    event.frame = 57; event.batchId = event.sequence = 1; event.eventValueName = "Gain"; event.value = 0.5;
    const auto referenceGraph = compileWholeCallStreamFixture(snapshot);
    OfflineRenderSession reference(referenceGraph);
    require(reference.scheduleMidiVoiceEvents({&event, 1}) && reference.render(41).ok(), "allocation oracle must commit its prefix");
    std::array<double, 17> beforeValues{};
    const auto before = referenceGraph.analysisCapture()->copyLatestRecordResult(0, beforeValues);
    allocationCount.store(0); countAllocations.store(true);
    const auto expected = reference.render(80);
    countAllocations.store(false);
    const auto sites = allocationCount.load();
    require(expected.ok() && sites > 38, "whole-call allocation sweep must include stream capture and output assembly");
    for (std::size_t index = 0; index != sites; ++index)
    {
        const auto graph = compileWholeCallStreamFixture(snapshot);
        OfflineRenderSession candidate(graph);
        require(candidate.scheduleMidiVoiceEvents({&event, 1}) && candidate.render(41).ok(), "allocation candidate must commit its prefix");
        std::array<double, 17> afterValues{};
        checkpointFaultCountdown = index; checkpointFaultTriggered = false;
        const auto failed = candidate.render(80);
        checkpointFaultCountdown = std::numeric_limits<std::size_t>::max();
        require(checkpointFaultTriggered && !failed.ok() && failed.mono.empty() && failed.channels.empty(),
            "every whole-call allocation fault must reject without partial output: " + std::to_string(index));
        const auto after = graph.analysisCapture()->copyLatestRecordResult(0, afterValues);
        require(before.status == after.status && before.header == after.header && beforeValues == afterValues,
            "allocation fault must preserve previously committed stream analysis: " + std::to_string(index));
        const auto retry = candidate.render(80);
        require(retry.ok() && retry.mono == expected.mono, "allocation retry must restore exact stream, input and DSP state");
    }
    std::cout << "P2 whole-call stream allocation failure/retry points: " << sites << '\n';
}

void testOfflineStreamRegistrySurvivesConcurrentInspection()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    std::vector<double> source(256);
    for (std::size_t index = 0; index != source.size(); ++index) source[index] = std::sin(index * 0.37) * 0.8;
    const auto snapshot = writeSnapshot(directory.path() / "whole-call-inspection.wav", 64.0, {source});
    const auto oracleOwner = prepareAudioStream(snapshot, manualConfiguration());
    require(oracleOwner.ok(), "session inspection source owner must prepare");
    const auto oracle = oracleOwner.owner->loadAllForOffline();
    require(oracle.ok(), "session inspection must use an independent decoded PCM oracle");
    const auto graph = compileWholeCallStreamFixture(snapshot);
    RealtimeRenderSession session(graph);
    require(session.ready(), "session inspection fixture must prepare");
    std::array<double, 7> prefix{};
    require(session.render(prefix), "session inspection fixture must commit a nonzero prefix");
    const auto baseline = session.streamHealth();
    require(baseline.consumerCount != 0 && baseline.consumers.size() == baseline.consumerCount,
        "session inspection fixture must expose its actual stream registry");
    std::atomic<std::size_t> observations = 0;
    std::atomic<bool> invalid = false;
    std::jthread observer([&](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            const auto current = session.streamHealth();
            bool same = current.consumerCount == baseline.consumerCount && current.consumers.size() == baseline.consumers.size();
            for (std::size_t index = 0; same && index != current.consumers.size(); ++index)
            {
                const auto& actual = current.consumers[index];
                const auto& expected = baseline.consumers[index];
                same = actual.nodeId == expected.nodeId && actual.structuralSourcePath == expected.structuralSourcePath
                    && actual.resourceId == expected.resourceId && actual.scopeId == expected.scopeId && actual.role == expected.role
                    && actual.seekGeneration == expected.seekGeneration && actual.health.bufferCapacity == expected.health.bufferCapacity;
            }
            if (!same) invalid.store(true, std::memory_order_relaxed);
            observations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (observations.load(std::memory_order_relaxed) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const auto beforeAttempts = observations.load(std::memory_order_relaxed);
    for (int attempt = 0; attempt != 60; ++attempt)
    {
        auto checkpoint = RealtimeRenderSessionTestAccess::checkpoint(session);
        require(bool(checkpoint), "streamed session must create a whole-call checkpoint");
        std::array<double, 17> provisional{};
        require(session.render(provisional), "streamed session must execute provisional audio");
        checkpoint.reset();
    }
    const auto afterAttempts = observations.load(std::memory_order_relaxed);
    observer.request_stop(); observer.join();
    require(beforeAttempts != 0 && afterAttempts > beforeAttempts && !invalid.load(),
        "session-level concurrent inspection must retain registry storage and immutable consumer identities across aborts");
    std::array<double, 17> retry{};
    require(session.render(retry), "session inspection fixture must remain renderable after repeated aborts");
    for (std::size_t index = 0; index != retry.size(); ++index)
        requireNear(retry[index], oracle.data->monoSamples[7 + index], "session inspection must not disturb exact audio continuation");
}

void testDeterministicDecodeAllocationFailuresRemainTyped()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    const auto prepared = prepareAudioStream(writeSnapshot(directory.path() / "offline-decode-fault.wav", 64,
        {std::vector<double>(128, -0.5)}), manualConfiguration());
    require(prepared.ok(), "decode fault owner prepares");
    auto consumer = prepared.owner->createConsumer(0);
    for (const std::size_t allocation : {0, 1})
    {
        checkpointFaultCountdown = allocation; checkpointFaultTriggered = false;
        bool badAllocation = false, escaped = false;
        try { auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer, true, true); }
        catch (const std::bad_alloc&) { badAllocation = true; }
        catch (...) { escaped = true; }
        checkpointFaultCountdown = std::numeric_limits<std::size_t>::max();
        require(checkpointFaultTriggered && badAllocation && !escaped,
            "deterministic decode allocation failure must retain the public render allocation exception type");
        auto retry = AudioStreamConsumerTestAccess::checkpoint(*consumer, true, true);
        require(bool(retry) && consumer->sourceFrameCursor() == 0,
            "decode allocation failure leaves the offline checkpoint retryable");
    }
}

void testOfflineParentStreamReadsBeyondPrewarmWithoutWorker()
{
    using namespace talas::runtime;
    using Access = AudioStreamConsumerTestAccess;
    TemporaryDirectory directory;
    std::vector<double> samples(128);
    for (std::size_t f = 0; f < samples.size(); ++f) samples[f] = std::sin(double(f) * 0.21);
    const auto prepared = prepareAudioStream(writeSnapshot(directory.path() / "offline-parent-horizon.wav", 64, {samples}), manualConfiguration());
    require(prepared.ok(), "offline parent horizon owner prepares");
    const auto consumer = prepared.owner->createConsumer(0);
    const auto oracle = prepared.owner->loadAllForOffline();
    require(bool(consumer) && oracle.ok(), "offline parent horizon oracle prepares");
    static_cast<void>(prepared.owner->pumpForTest());
    for (const bool commit : {false, true})
    {
        auto checkpoint = Access::checkpoint(*consumer, true, true);
        require(bool(checkpoint), "deterministic offline participant begins");
        bool exact = true, begun = true;
        allocationCount = 0; countAllocations = true;
        for (std::size_t offset = 0; offset < 64; offset += 8)
        {
            begun = begun && Access::beginCallback(*consumer);
            for (std::size_t f = 0; f < 8; ++f)
            {
                const auto result = consumer->readNext(2);
                exact = exact && result.status == AudioStreamReadStatus::Available
                    && result.sample == oracle.data->monoSamples[(offset + f) * 2];
            }
            Access::commitCallback(*consumer);
        }
        countAllocations = false;
        require(begun && exact && allocationCount == 0,
            "offline source must read beyond prewarm at a transposed rate without worker progress or callback allocation");
        if (commit) checkpoint->commit();
        checkpoint.reset();
        require(consumer->sourceFrameCursor() == (commit ? 128 : 0),
            "whole offline boundary restores/commits the stream cursor across callbacks");
    }
}

void testCallbackConsumerRetainsResponsesAcrossRollback()
{
    using namespace talas::runtime;
    using Access = AudioStreamConsumerTestAccess;
    TemporaryDirectory directory;
    std::vector<double> samples(64);
    for (std::size_t f = 0; f < samples.size(); ++f) samples[f] = (double(f) - 32) / 64;
    const auto prepared = prepareAudioStream(writeSnapshot(directory.path() / "callback-rollback.wav", 64, {samples}), manualConfiguration());
    require(prepared.ok(), "callback source prepares");
    const auto oracle = prepared.owner->loadAllForOffline();
    require(oracle.ok(), "callback oracle decodes exact PCM");
    const auto consumer = prepared.owner->createConsumer(0);
    static_cast<void>(prepared.owner->pumpForTest());
    const auto generation = consumer->seekGeneration();
    const auto buffered = consumer->health().bufferedBlocks;
    std::array<double, 4> tentative{}, retry{};
    allocationCount = 0; countAllocations = true;
    const bool started = Access::beginCallback(*consumer);
    const bool nestedRejected = !Access::beginCallback(*consumer);
    for (auto& sample : tentative) sample = consumer->readNext(1).sample;
    const auto pendingBuffered = consumer->health().bufferedBlocks;
    Access::rollbackCallback(*consumer);
    const auto restoredCursor = consumer->sourceFrameCursor();
    const bool restarted = Access::beginCallback(*consumer);
    for (auto& sample : retry) sample = consumer->readNext(1).sample;
    Access::commitCallback(*consumer);
    countAllocations = false;
    require(started && nestedRejected && restarted, "callback checkpoint must begin and reject overlap");
    require(allocationCount == 0 && restoredCursor == 0 && consumer->sourceFrameCursor() == 4,
        "callback rollback and commit must restore/publish exact cursor without allocation");
    require(pendingBuffered == buffered && tentative == retry && retry[0] == oracle.data->monoSamples[0] && retry[3] == oracle.data->monoSamples[3],
        "tentative reads must retain queue slots and exact samples through retry");
    require(consumer->seekGeneration() == generation && consumer->health().bufferedBlocks < buffered,
        "commit consumes retained responses without inventing a seek");

    const auto baselineCursor = consumer->sourceFrameCursor();
    const auto baselineHealth = consumer->health();
    require(Access::beginCallback(*consumer), "seek checkpoint begins");
    require(consumer->seek(40), "tentative seek succeeds");
    require(consumer->readNext(1).status == AudioStreamReadStatus::Missing, "unprepared seek remains a bounded miss");
    require(consumer->seekGeneration() == generation && consumer->health().missingFrames == baselineHealth.missingFrames,
        "tentative seek and missing counters are unpublished");
    static_cast<void>(prepared.owner->pumpForTest());
    Access::rollbackCallback(*consumer);
    require(consumer->sourceFrameCursor() == baselineCursor && consumer->seekGeneration() == generation,
        "worker progress must not invalidate abort cursor/generation");
    require(Access::beginCallback(*consumer) && consumer->seek(40), "committed seek begins");
    Access::commitCallback(*consumer);
    static_cast<void>(prepared.owner->pumpForTest());
    require(consumer->seekGeneration() > generation, "commit publishes seek epoch");
    requireNear(consumer->readNext(1).sample, oracle.data->monoSamples[40], "worker receives committed refill");
}

void testOfflineConsumerDefersInspectionUntilCommit()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(directory.path() / "checkpoint-inspection.wav", 64.0, {std::vector<double>(64, 0.5)});
    const auto prepared = prepareAudioStream(snapshot, manualConfiguration());
    require(prepared.ok(), "inspection owner must prepare");
    const auto consumer = prepared.owner->createConsumer(3.5);
    require(bool(consumer), "inspection consumer must prepare");
    static_cast<void>(prepared.owner->pumpForTest());
    const auto health = consumer->health();
    const auto generation = consumer->seekGeneration();
    for (const bool commit : {false, true})
    {
        auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer, true);
        require(consumer->seek(55.0, -1.0), "inspection fixture must execute a provisional seek");
        for (int index = 0; index != 10; ++index) static_cast<void>(consumer->readNext(-1.0));
        static_cast<void>(prepared.owner->pumpForTest());
        allocationCount.store(0); countAllocations.store(true);
        const auto retainedHealth = consumer->health();
        const auto retainedGeneration = consumer->seekGeneration();
        countAllocations.store(false);
        require(allocationCount.load() == 0 && retainedGeneration == generation && consumer->sourceFrameCursor() == 45.0,
            "control inspection must allocate nothing and retain authority while the processing cursor advances");
        requireSameHealth(retainedHealth, health);
        allocationCount.store(0); countAllocations.store(true);
        if (commit) checkpoint->commit();
        checkpoint.reset();
        countAllocations.store(false);
        require(allocationCount.load() == 0, "stream commit and abort must allocate nothing");
        if (commit)
            require(consumer->seekGeneration() == generation + 1 && consumer->health().missingFrames > health.missingFrames,
                "commit must expose retained processing generation and health");
        else
        {
            require(consumer->seekGeneration() == generation && consumer->sourceFrameCursor() == 3.5,
                "abort must restore cursor and generation before releasing inspection");
            requireSameHealth(consumer->health(), health);
        }
    }
}

void testOfflineConsumerCheckpointOwnsWorkerAndCursorState()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    std::vector<double> source(64);
    for (std::size_t index = 0; index != source.size(); ++index)
        source[index] = std::sin(static_cast<double>(index) * 0.37) * 0.8;
    const auto snapshot = writeSnapshot(directory.path() / "checkpoint.wav", 64.0, {source});
    const auto prepared = prepareAudioStream(snapshot, manualConfiguration());
    const auto referenceOwner = prepareAudioStream(snapshot, manualConfiguration());
    require(prepared.ok() && referenceOwner.ok(), "checkpoint owners must prepare");
    const auto full = prepared.owner->loadAllForOffline();
    require(full.ok(), "checkpoint must have an independent immutable audio oracle");
    for (const double increment : {0.5, -0.5})
    {
        const auto start = increment > 0 ? 3.25 : 45.25;
        auto consumer = prepared.owner->createConsumer(start, increment);
        auto reference = referenceOwner.owner->createConsumer(start, increment);
        auto unrelated = prepared.owner->createConsumer(20.0);
        require(consumer && reference && unrelated, "checkpoint consumers must prepare");
        auto budget = preparedWindowBudget();
        auto referenceBudget = preparedWindowBudget();
        require(prepared.owner->prepareRestartWindow(consumer, 12.0, 1.0, 4, budget)
            && prepared.owner->prepareReadWindow(consumer, 30.0, -1.0, 4, budget)
            && referenceOwner.owner->prepareRestartWindow(reference, 12.0, 1.0, 4, referenceBudget)
            && referenceOwner.owner->prepareReadWindow(reference, 30.0, -1.0, 4, referenceBudget),
            "checkpoint must preserve independently prepared identities");
        static_cast<void>(prepared.owner->pumpForTest());
        static_cast<void>(referenceOwner.owner->pumpForTest());
        require(consumer->readNext(increment).sample == reference->readNext(increment).sample,
            "checkpoint must start after a committed fractional sample");
        const auto cursor = consumer->sourceFrameCursor();
        const auto generation = consumer->seekGeneration();
        const auto health = consumer->health();
        for (int attempt = 0; attempt != 2; ++attempt)
        {
            auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer);
            require(bool(checkpoint), "exclusive stream checkpoint must prepare");
            bool nestedRejected = false;
            try { auto nested = AudioStreamConsumerTestAccess::checkpoint(*consumer); }
            catch (const std::logic_error&) { nestedRejected = true; }
            require(nestedRejected, "nested stream checkpoints must be rejected without mutation");
            require(consumer->seek(57.0, -1.0), "provisional seek must advance generation");
            for (int frame = 0; frame != 12; ++frame) static_cast<void>(consumer->readNext(-1.0));
            require(consumer->health().missingFrames > health.missingFrames, "fixture must overwrite underrun state");
            static_cast<void>(prepared.owner->pumpForTest());
            require(consumer->restartPrepared(12.0, 1.0) && consumer->relocatePrepared(30.0, -1.0),
                "provisional epoch mailbox must replace stale requests");
            static_cast<void>(prepared.owner->pumpForTest());
            static_cast<void>(unrelated->readNext(1.0));
            const auto unrelatedCursor = unrelated->sourceFrameCursor();
            const auto unrelatedGeneration = unrelated->seekGeneration();
            const auto unrelatedHealth = unrelated->health();
            allocationCount.store(0); countAllocations.store(true);
            checkpoint.reset();
            countAllocations.store(false);
            require(allocationCount.load() == 0 && consumer->sourceFrameCursor() == cursor
                && consumer->seekGeneration() == generation, "stream restore must allocate nothing and restore exact cursor/generation");
            requireSameHealth(consumer->health(), health);
            require(unrelated->sourceFrameCursor() == unrelatedCursor && unrelated->seekGeneration() == unrelatedGeneration,
                "stream rollback must not rewind another consumer sharing the owner");
            requireSameHealth(unrelated->health(), unrelatedHealth);
        }
        std::size_t availableFrames = 0;
        for (int frame = 0; frame != 32; ++frame)
        {
            static_cast<void>(prepared.owner->pumpForTest());
            static_cast<void>(referenceOwner.owner->pumpForTest());
            const auto position = consumer->sourceFrameCursor();
            const auto actual = consumer->readNext(increment);
            const auto expected = reference->readNext(increment);
            require(actual.status == expected.status && actual.sample == expected.sample,
                "restored worker continuation must match an independently pumped reference: frame=" + std::to_string(frame)
                + " position=" + std::to_string(position) + " increment=" + std::to_string(increment)
                + " actual=" + std::to_string(static_cast<int>(actual.status)) + "/" + std::to_string(actual.sample)
                + " expected=" + std::to_string(static_cast<int>(expected.status)) + "/" + std::to_string(expected.sample));
            if (actual.status == AudioStreamReadStatus::Available)
            {
                ++availableFrames;
                const auto left = static_cast<std::size_t>(std::floor(position));
                const auto fraction = position - static_cast<double>(left);
                requireNear(actual.sample, full.data->monoSamples[left]
                    + (full.data->monoSamples[left + 1] - full.data->monoSamples[left]) * fraction,
                    "restored stream must match independent fractional interpolation");
            }
            else require(actual.status == AudioStreamReadStatus::Missing && actual.sample == 0.0,
                "the bounded manual stream must retain its exact missing-data policy");
        }
        require(availableFrames != 0, "continuation must include decoded audio, not only missing frames");
        auto committed = AudioStreamConsumerTestAccess::checkpoint(*consumer);
        require(consumer->seek(18.0), "committed checkpoint must retain its seek");
        const auto committedGeneration = consumer->seekGeneration();
        committed->commit(); committed.reset();
        require(consumer->sourceFrameCursor() == 18.0 && consumer->seekGeneration() == committedGeneration,
            "committed checkpoint must retain processing state");
        auto next = AudioStreamConsumerTestAccess::checkpoint(*consumer);
        next.reset();
    }
}

void testOfflineConsumerCheckpointPreservesFreshAndTerminalBoundaries()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(directory.path() / "checkpoint-boundaries.wav", 64.0, {{0.0, 0.25, 0.5, -0.5}});
    const auto prepared = prepareAudioStream(snapshot, manualConfiguration());
    require(prepared.ok(), "boundary checkpoint owner must prepare");
    const auto consumer = prepared.owner->createConsumer(0.0);
    require(bool(consumer), "fresh checkpoint consumer must prepare");
    const auto freshHealth = consumer->health();
    {
        auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer);
        static_cast<void>(prepared.owner->pumpForTest());
        for (int index = 0; index != 4; ++index) static_cast<void>(consumer->readNext(1.0));
        require(consumer->readNext(1.0).status == AudioStreamReadStatus::EndOfStream,
            "fresh checkpoint fixture must overwrite through end-of-stream");
    }
    require(consumer->sourceFrameCursor() == 0.0 && consumer->seekGeneration() == 1,
        "fresh checkpoint must restore its initial cursor and generation");
    requireSameHealth(consumer->health(), freshHealth);
    static_cast<void>(prepared.owner->pumpForTest());
    for (int index = 0; index != 4; ++index)
        require(consumer->readNext(1.0).status == AudioStreamReadStatus::Available,
            "fresh restored request must refill all original frames");
    const auto terminalHealth = consumer->health();
    {
        auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer);
        require(consumer->seek(0.0), "terminal checkpoint fixture must overwrite its epoch");
        static_cast<void>(prepared.owner->pumpForTest());
        static_cast<void>(consumer->readNext(1.0));
    }
    requireSameHealth(consumer->health(), terminalHealth);
    require(consumer->sourceFrameCursor() == 4.0 && consumer->seekGeneration() == 1
        && consumer->readNext(1.0).status == AudioStreamReadStatus::EndOfStream,
        "terminal checkpoint must restore exact end-of-stream without restarting playback");
}

void testOfflineConsumerCheckpointAllocationFailureIsAtomic()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(directory.path() / "checkpoint-allocation.wav", 64.0,
        {std::vector<double>(64, 0.5)});
    const auto prepared = prepareAudioStream(snapshot, manualConfiguration());
    require(prepared.ok(), "allocation checkpoint owner must prepare");
    const auto consumer = prepared.owner->createConsumer(3.5);
    auto budget = preparedWindowBudget();
    require(consumer && prepared.owner->prepareRestartWindow(consumer, 12.0, 1.0, 6, budget),
        "allocation checkpoint must include retained response payloads");
    static_cast<void>(prepared.owner->pumpForTest());
    const auto health = consumer->health();
    const auto generation = consumer->seekGeneration();
    for (const bool deferInspection : {false, true})
    {
        allocationCount.store(0); countAllocations.store(true);
        auto measured = AudioStreamConsumerTestAccess::checkpoint(*consumer, deferInspection);
        const auto sites = allocationCount.load();
        countAllocations.store(false);
        measured->commit(); measured.reset();
        require(sites != 0, "owned stream checkpoint must exercise allocating capture");
        for (std::size_t index = 0; index != sites; ++index)
        {
            checkpointFaultCountdown = index;
            checkpointFaultTriggered = false;
            bool rejected = false;
            try { auto faulted = AudioStreamConsumerTestAccess::checkpoint(*consumer, deferInspection); }
            catch (const std::bad_alloc&) { rejected = true; }
            checkpointFaultCountdown = std::numeric_limits<std::size_t>::max();
            require(rejected && checkpointFaultTriggered && consumer->sourceFrameCursor() == 3.5
                && consumer->seekGeneration() == generation, "failed checkpoint allocation must preserve cursor and generation");
            requireSameHealth(consumer->health(), health);
            auto retry = AudioStreamConsumerTestAccess::checkpoint(*consumer, deferInspection);
            require(consumer->seek(31.0), "allocation retry must allow provisional processing");
            static_cast<void>(prepared.owner->pumpForTest());
            retry.reset();
            require(consumer->sourceFrameCursor() == 3.5 && consumer->seekGeneration() == generation,
                "allocation failure must leave no active-checkpoint flag or partial baseline");
            requireSameHealth(consumer->health(), health);
        }
        std::cout << "P2 stream checkpoint allocation failure/retry points: " << sites << " (deferred=" << deferInspection << ")" << '\n';
    }
}

void testOfflineConsumerCheckpointSynchronizesBackgroundWorker()
{
    using namespace talas::runtime;
    TemporaryDirectory directory;
    std::vector<double> source(4096);
    for (std::size_t index = 0; index != source.size(); ++index)
        source[index] = std::sin(static_cast<double>(index) * 0.17) * 0.8;
    const auto snapshot = writeSnapshot(directory.path() / "checkpoint-background.wav", 48000.0, {source});
    auto configuration = manualConfiguration();
    configuration.workerMode = AudioStreamWorkerMode::Background;
    configuration.blockFrames = 16;
    const auto prepared = prepareAudioStream(snapshot, configuration);
    require(prepared.ok(), "background checkpoint owner must prepare");
    const auto consumer = prepared.owner->createConsumer(7.5, 0.5);
    const auto full = prepared.owner->loadAllForOffline();
    require(consumer && full.ok(), "background checkpoint consumer and immutable oracle must prepare");
    std::atomic<std::uint64_t> observations = 0;
    std::jthread observer([&](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            const auto health = consumer->health();
            static_cast<void>(consumer->seekGeneration());
            if (health.bufferCapacity == configuration.responseCapacity
                && health.highWatermark == configuration.highWatermark)
                observations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });
    for (int iteration = 0; iteration != 30; ++iteration)
    {
        require(prepared.owner->prewarm(consumer, std::chrono::milliseconds{1000}),
            "background checkpoint must start with a warm immutable source window");
        const auto cursor = consumer->sourceFrameCursor();
        const auto generation = consumer->seekGeneration();
        const bool deferInspection = iteration % 2 == 0;
        auto checkpoint = AudioStreamConsumerTestAccess::checkpoint(*consumer, deferInspection);
        const auto retainedHealth = consumer->health();
        const auto decoded = retainedHealth.decodedBlocks;
        require(consumer->seek(300.0 + iteration * 17.0, -1.0), "background provisional seek must advance refill epoch");
        if (deferInspection)
        {
            require(prepared.owner->prewarm(consumer, std::chrono::milliseconds{1000}),
                "worker must refill the provisional reverse window while inspection is deferred");
            requireSameHealth(consumer->health(), retainedHealth);
            require(consumer->seekGeneration() == generation, "background provisional generation must remain private");
        }
        else
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
            while (consumer->health().decodedBlocks == decoded && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            require(consumer->health().decodedBlocks > decoded, "background worker must actually mutate response state after capture");
        }
        for (int frame = 0; frame != 10; ++frame) static_cast<void>(consumer->readNext(-1.0));
        checkpoint.reset();
        require(consumer->sourceFrameCursor() == cursor && consumer->seekGeneration() == generation,
            "background restore must retain the committed consumer identity and generation");
        const auto sample = consumer->readNext(0.5);
        require(sample.status == AudioStreamReadStatus::Available, "restored response payload must be immediately readable");
        const auto left = static_cast<std::size_t>(std::floor(cursor));
        requireNear(sample.sample, full.data->monoSamples[left]
            + (full.data->monoSamples[left + 1] - full.data->monoSamples[left]) * (cursor - left),
            "background restore must reject stale refill payloads and match independent interpolation");
    }
    observer.request_stop(); observer.join();
    require(observations.load() != 0, "a concurrent health reader must run during background checkpoint activity");
}

void testManualStreamDecodesMonoAndStereoReduction()
{
    TemporaryDirectory directory;
    const auto mono = writeSnapshot(
        directory.path() / "mono.wav",
        48000.0,
        {{0.0, 0.5, -1.0, 1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        mono,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);
    require(consumer != nullptr, "mono consumer should prepare");
    const auto waitingHealth = consumer->health();
    require(
        waitingHealth.bufferedBlocks == 0
            && waitingHealth.bufferCapacity == 4
            && waitingHealth.lowWatermark == 1
            && waitingHealth.highWatermark == 3
            && waitingHealth.pendingRequests == 1
            && waitingHealth.workerState
                == talas::runtime::AudioStreamWorkerState::Waiting,
        "new consumer telemetry should expose exact empty fill, watermarks, request pressure, and waiting worker state");
    require(
        prepared.owner->pumpForTest() >= 1,
        "manual worker should publish decoded mono blocks");
    const auto readyHealth = consumer->health();
    require(
        readyHealth.bufferedBlocks == 2
            && readyHealth.bufferCapacity == 4
            && readyHealth.pendingRequests == 0
            && readyHealth.workerState
                == talas::runtime::AudioStreamWorkerState::EndOfStream,
        "short stream telemetry should expose exact prewarmed fill and terminal worker state");

    requireNear(
        consumer->readNext(1.0).sample,
        0.0,
        "mono frame zero should decode");
    requireNear(
        consumer->readNext(1.0).sample,
        16384.0 / 32767.0,
        "mono positive PCM16 should use the reviewed normalization");
    requireNear(
        consumer->readNext(1.0).sample,
        -1.0,
        "mono negative PCM16 should decode");

    const auto stereo = writeSnapshot(
        directory.path() / "stereo.wav",
        48000.0,
        {
            {1.0, 0.5, -1.0, 0.0},
            {-1.0, 0.5, 1.0, 0.0},
        });
    auto stereoPrepared = talas::runtime::prepareAudioStream(
        stereo,
        manualConfiguration());
    require(stereoPrepared.ok(), stereoPrepared.error);
    auto stereoConsumer =
        stereoPrepared.owner->createConsumer(0.0);
    require(stereoConsumer != nullptr, "stereo consumer should prepare");
    (void) stereoPrepared.owner->pumpForTest();
    requireNear(
        stereoConsumer->readNext(1.0).sample,
        0.0,
        "opposed stereo frame should reduce to mono midpoint");
    requireNear(
        stereoConsumer->readNext(1.0).sample,
        16384.0 / 32767.0,
        "matching stereo frame should preserve its midpoint");
}

// A DiskPlayer streams a recording imported from another program in any of
// the documented formats: WAV of other word sizes
// and AIFF, each decoded as the stream reads it.
void testStreamDecodesWavAndAiffSamplesOfOtherWordSizes()
{
    TemporaryDirectory directory;
    const auto decoded = [&](const std::string& name,
                             const std::vector<std::uint8_t>& bytes,
                             std::uint32_t channels,
                             std::uint64_t frames) {
        auto prepared = talas::runtime::prepareAudioStream(
            writeSampleFileSnapshot(directory.path() / name, bytes, channels, frames),
            manualConfiguration());
        require(prepared.ok(), name + " should prepare: " + prepared.error);
        const auto full = prepared.owner->loadAllForOffline();
        require(full.ok(), name + " should decode: " + full.error);
        return full.data->monoSamples;
    };
    // 24-bit stereo: full scale either way, then half scale twice.
    const auto twentyFour = decoded("stereo24.wav",
        sampleWave(1, 2, 24, {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x00, 0x00, 0x40, 0x00, 0x00, 0x40}),
        2, 2);
    require(twentyFour.size() == 2 && twentyFour[0] == 0.0
            && twentyFour[1] == 4194304.0 / 8388607.0,
        "24-bit stereo WAV frames should reduce to their midpoints");
    const auto eight = decoded("mono8.wav", sampleWave(1, 1, 8, {255, 0, 128}), 1, 3);
    require(eight == std::vector<double>{1.0, -1.0, 0.0},
        "8-bit WAV samples should decode around 128");
    const auto floats = decoded("float.wav",
        sampleWave(3, 1, 32, {0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0x40, 0xBF}), 1, 2);
    require(floats == std::vector<double>{0.25, -0.75},
        "32-bit float WAV samples should decode exactly");
    const auto aiff = decoded("mono16.aiff", sampleAiff(1, 16, {0x40, 0x00, 0x80, 0x00}), 1, 2);
    require(aiff == std::vector<double>{16384.0 / 32767.0, -1.0},
        "16-bit AIFF samples should decode big-endian");

    // A file claimed as canonical PCM16 WAV is still read only as one.
    auto canonical = std::make_shared<talas::core::ResourceSnapshot>(
        *writeSampleFileSnapshot(directory.path() / "claimed.wav",
            sampleWave(1, 1, 24, {0x00, 0x00, 0x40}), 1, 1));
    canonical->encoding = talas::project::pcm16WavAssetEncoding;
    require(!talas::runtime::prepareAudioStream(canonical, manualConfiguration()).ok(),
        "a 24-bit file claimed as PCM16 WAV should be refused");
    const auto surround = talas::runtime::prepareAudioStream(
        writeSampleFileSnapshot(directory.path() / "surround.wav",
            sampleWave(1, 3, 16, {0, 0, 0, 0, 0, 0}), 3, 1),
        manualConfiguration());
    require(!surround.ok(), "a stream should refuse more than two channels");
}

void testFailedBlockReadDoesNotCacheSilence()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "partial.wav";
    // An unusual rate and level identify this test's private backing copy.
    const auto snapshot = writeSnapshot(
        source,
        12'347.0,
        {{0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}});
    std::ifstream sourceInput(source, std::ios::binary);
    const std::string sourceBytes{
        std::istreambuf_iterator<char>{sourceInput},
        std::istreambuf_iterator<char>{}};
    auto configuration = manualConfiguration();
    configuration.decodedCacheBlocks = 1;
    auto prepared = talas::runtime::prepareAudioStream(snapshot, configuration);
    require(prepared.ok(), prepared.error);

    std::filesystem::path backing;
    for (const auto& entry : std::filesystem::directory_iterator(
             std::filesystem::temp_directory_path()))
    {
        const auto candidate = entry.path() / "generation.wav";
        if (!entry.path().filename().string().starts_with("talas-audio-stream-")
            || !std::filesystem::is_regular_file(candidate))
        {
            continue;
        }
        std::ifstream input(candidate, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
        if (bytes == sourceBytes)
        {
            backing = candidate;
            break;
        }
    }
    require(!backing.empty(),
        "the stream's private backing copy should be found");
    // Keep the header and block zero with its interpolation frame, so every
    // later block read fails.
    std::filesystem::resize_file(backing, 44U + 3U * 2U);

    auto first = prepared.owner->createConsumer(0.0);
    require(first != nullptr, "first consumer should prepare");
    (void) prepared.owner->pumpForTest();
    auto second = prepared.owner->createConsumer(0.0);
    require(second != nullptr, "second consumer should prepare");
    (void) prepared.owner->pumpForTest();
    const auto read = second->readNext(1.0);
    require(read.status == talas::runtime::AudioStreamReadStatus::Available
            && std::abs(read.sample - 0.5) < 1.0e-4,
        "a failed later read must not leave block zero cached as silence");
}

void testConsumersSeekIndependentlyAndDiscardStaleGenerations()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "independent.wav",
        48000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto first = prepared.owner->createConsumer(0.0);
    auto second = prepared.owner->createConsumer(0.0);
    (void) prepared.owner->pumpForTest();

    require(first->seek(4.0), "first consumer seek should publish");
    require(
        first->seekGeneration() == 2
            && second->seekGeneration() == 1,
        "one consumer seek must not advance another consumer generation");
    (void) prepared.owner->pumpForTest();
    const auto firstSample = first->readNext(1.0);
    const auto secondSample = second->readNext(1.0);
    require(
        firstSample.status
                == talas::runtime::AudioStreamReadStatus::Available
            && secondSample.status
                == talas::runtime::AudioStreamReadStatus::Available,
        "both independent generations should receive prepared data");
    requireNear(
        firstSample.sample,
        1.0,
        "first consumer should read its seek target");
    requireNear(
        secondSample.sample,
        0.0,
        "second consumer cursor should remain at its original target");
    require(
        first->health().staleResponseBlocks >= 1,
        "consumer should count response blocks discarded from its older seek generation");
}

void testPreparedRestartIsExactImmediateAndNotAnOrdinarySeek()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "prepared-restart.wav",
        48000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0, 1.0);
    require(consumer != nullptr, "restart consumer should prepare");
    (void) prepared.owner->pumpForTest();

    require(
        !consumer->restartPrepared(0.0, 1.0),
        "restart must fail before a control-thread window is bound");
    require(
        [&] {
            auto budget = preparedWindowBudget();
            return prepared.owner->prepareRestartWindow(
                consumer,
                0.0,
                1.0,
                1,
                budget);
        }(),
        "control thread should bind one exact restart window");
    require(
        !consumer->restartPrepared(1.0, 1.0)
            && !consumer->restartPrepared(0.0, -1.0),
        "restart must reject a different cursor or direction");

    const auto generation = consumer->seekGeneration();
    require(
        consumer->restartPrepared(0.0, 1.0)
            && consumer->seekGeneration() == generation,
        "prepared restart should not publish an ordinary seek generation");
    const auto restarted = consumer->readNext(1.0);
    require(
        restarted.status
                == talas::runtime::AudioStreamReadStatus::Available
            && consumer->health().missingFrames == 0
            && consumer->health().underrunEvents == 0,
        "prepared restart should serve its retained window without a worker pump");
    requireNear(
        restarted.sample,
        0.0,
        "prepared restart should return the exact bound start sample");
}

void testPreparedRelocationReplacesTheStaleQueueWithoutAnOrdinarySeek()
{
    TemporaryDirectory directory;
    std::vector<double> samples(40, 0.0);
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<double>(frame) / 40.0;
    const auto snapshot = writeSnapshot(
        directory.path() / "prepared-relocation.wav",
        48000.0,
        {samples});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(24.0, 1.0);
    require(consumer != nullptr, "relocation consumer should prepare");
    require(
        prepared.owner->pumpForTest() == 3,
        "initial stream should retain its later ordinary queue");

    require(
        !consumer->relocatePrepared(2.0, 1.0),
        "relocation must fail before a control-thread endpoint is bound");
    require(
        [&] {
            auto budget = preparedWindowBudget();
            return prepared.owner->prepareReadWindow(
                consumer,
                2.0,
                1.0,
                6,
                budget);
        }(),
        "control thread should retain and bind the earlier endpoint");
    require(
        !consumer->relocatePrepared(3.0, 1.0)
            && !consumer->relocatePrepared(2.0, -1.0),
        "relocation must reject a different endpoint or direction");

    const auto generation = consumer->seekGeneration();
    require(
        consumer->relocatePrepared(2.0, 1.0)
            && consumer->seekGeneration() == generation,
        "prepared relocation should replace the refill epoch without publishing an ordinary seek");
    require(
        prepared.owner->pumpForTest() == 3,
        "relocation should free the stale later queue for an immediate earlier refill");
    for (std::size_t frame = 2; frame < 8; ++frame)
    {
        const auto result = consumer->readNext(1.0);
        require(
            result.status == talas::runtime::AudioStreamReadStatus::Available,
            "relocated prepared and ordinary windows should remain contiguous");
        require(
            std::abs(
                result.sample
                - static_cast<double>(frame) / 40.0)
                <= 2.0e-4,
            "relocated sample " + std::to_string(frame));
    }
    require(
        prepared.owner->pumpForTest() > 0,
        "consuming the relocated queue should permit its next bounded refill");
    require(
        consumer->readNext(1.0).status
            == talas::runtime::AudioStreamReadStatus::Available,
        "relocated refill should continue beyond the retained endpoint window");
}

void testPreparedWindowCoversTheMaximumTransposedCallback()
{
    TemporaryDirectory directory;
    std::vector<double> samples(8192, 0.0);
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = 0.125 + static_cast<double>(frame % 97U) / 256.0;
    const auto snapshot = writeSnapshot(
        directory.path() / "prepared-transposed-window.wav",
        48000.0,
        {samples});
    auto configuration = manualConfiguration();
    configuration.blockFrames = 512;
    configuration.requestCapacity = 8;
    configuration.responseCapacity = 8;
    configuration.decodedCacheBlocks = 8;
    configuration.lowWatermark = 2;
    configuration.highWatermark = 6;
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        configuration);
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0, 8.0);
    require(consumer != nullptr, "transposed consumer should prepare");
    auto budget = preparedWindowBudget();
    require(
        prepared.owner->prepareRestartWindow(
            consumer,
            0.0,
            8.0,
            512,
            budget),
        "control thread should retain one maximum transposed callback");
    require(
        consumer->restartPrepared(0.0, 8.0),
        "transposed restart should match the prepared identity");

    for (std::size_t outputFrame = 0; outputFrame < 512; ++outputFrame)
    {
        const auto sourceFrame = static_cast<double>(outputFrame * 8U);
        const auto result = consumer->readInterpolated(
            sourceFrame,
            sourceFrame + 1.0,
            sourceFrame + 8.0,
            8.0);
        require(
            result.status == talas::runtime::AudioStreamReadStatus::Available,
            "prepared high-pitch callback must not race the stream worker at output frame "
                + std::to_string(outputFrame));
    }
    require(
        consumer->health().missingFrames == 0
            && consumer->health().underrunEvents == 0,
        "one admitted transposed callback should be completely retained");
}

void testPreparedEpochMailboxSupersedesAFullStaleRequestQueue()
{
    TemporaryDirectory directory;
    std::vector<double> samples(100, 0.0);
    for (std::size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<double>(frame) / 100.0;
    const auto snapshot = writeSnapshot(
        directory.path() / "prepared-epoch-mailbox.wav",
        48000.0,
        {samples});

    const auto run = [&](bool restart) {
        auto configuration = manualConfiguration();
        configuration.highWatermark = 1;
        configuration.lowWatermark = 1;
        auto prepared = talas::runtime::prepareAudioStream(
            snapshot,
            configuration);
        require(prepared.ok(), prepared.error);
        auto consumer = prepared.owner->createConsumer(20.0, 1.0);
        require(consumer != nullptr, "epoch mailbox consumer should prepare");
        auto budget = preparedWindowBudget();
        const auto windowPrepared = restart
            ? prepared.owner->prepareRestartWindow(
                  consumer,
                  0.0,
                  1.0,
                  1,
                  budget)
            : prepared.owner->prepareReadWindow(
                  consumer,
                  0.0,
                  1.0,
                  1,
                  budget);
        require(windowPrepared, "epoch mailbox endpoint should prepare");

        for (std::size_t request = 0; request < 3; ++request)
        {
            const auto sourceFrame = 20.0
                + static_cast<double>(request * 2U);
            (void) consumer->readInterpolated(
                sourceFrame,
                sourceFrame + 1.0,
                sourceFrame + 2.0,
                1.0);
        }
        const auto generation = consumer->seekGeneration();
        const auto repositioned = restart
            ? consumer->restartPrepared(0.0, 1.0)
            : consumer->relocatePrepared(0.0, 1.0);
        require(
            repositioned
                && consumer->seekGeneration() == generation,
            "prepared epoch should supersede the full stale request queue without an ordinary seek");
        require(
            prepared.owner->pumpForTest(1) == 1,
            "manual worker should consume the guaranteed latest prepared epoch");
        const auto retainedEndpoint = consumer->readInterpolated(
            0.0,
            1.0,
            2.0,
            1.0);
        require(
            retainedEndpoint.status
                    == talas::runtime::AudioStreamReadStatus::Available
                && consumer->health().staleResponseBlocks == 0,
            "latest epoch should publish a current-generation response instead of one stale queued request");
        require(
            prepared.owner->pumpForTest(1) == 1,
            "current-generation cursor progress should refill the next block");
        const auto beyondRetainedEndpoint = consumer->readInterpolated(
            2.0,
            3.0,
            3.0,
            1.0);
        require(
            beyondRetainedEndpoint.status
                == talas::runtime::AudioStreamReadStatus::Available,
            restart
                ? "restart mailbox should refill beyond its retained block"
                : "release relocation mailbox should refill beyond its retained block");
    };

    run(true);
    run(false);
}

void testPreparedWindowBudgetFailsBeforeAllocationAndChargesUniqueBlocks()
{
    TemporaryDirectory directory;
    std::vector<double> samples(40, 0.25);
    const auto snapshot = writeSnapshot(
        directory.path() / "prepared-window-budget.wav",
        48000.0,
        {samples});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0, 1.0);
    require(consumer != nullptr, "budgeted consumer should prepare");

    talas::runtime::AudioStreamPreparedWindowBudget rejectedBudget{
        2,
        1024U * 1024U,
        0,
        0,
    };
    require(
        !prepared.owner->prepareReadWindow(
            consumer,
            0.0,
            8.0,
            1,
            rejectedBudget)
            && rejectedBudget.usedBlocks == 0
            && rejectedBudget.usedBytes == 0
            && !consumer->relocatePrepared(0.0, 1.0),
        "insufficient aggregate budget should fail before allocation or identity publication");

    auto acceptedBudget = preparedWindowBudget();
    require(
        prepared.owner->prepareReadWindow(
            consumer,
            0.0,
            8.0,
            1,
            acceptedBudget)
            && acceptedBudget.usedBlocks == 5
            && acceptedBudget.usedBytes > 0,
        "sufficient budget should charge every unique retained block");
    const auto chargedBlocks = acceptedBudget.usedBlocks;
    const auto chargedBytes = acceptedBudget.usedBytes;
    require(
        prepared.owner->prepareReadWindow(
            consumer,
            0.0,
            8.0,
            1,
            acceptedBudget)
            && acceptedBudget.usedBlocks == chargedBlocks
            && acceptedBudget.usedBytes == chargedBytes,
        "reusing one exact retained window should not consume aggregate budget twice");
    require(
        consumer->readInterpolated(8.0, 9.0, 9.0, 1.0).status
            == talas::runtime::AudioStreamReadStatus::Available,
        "budgeted prepared lookup should find the far retained block");
}

void testLinearSourceRateConversionMatchesFullLoadOracle()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "resample.wav",
        24000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);
    require(
        prepared.owner->pumpForTest() == 3,
        "manual prewarm should publish the configured high watermark");

    const std::vector<double> decoded = {
        0.0,
        8192.0 / 32767.0,
        16384.0 / 32767.0,
        24575.0 / 32767.0,
        1.0,
        -1.0,
    };
    const auto fullLoadLinear = [&](double position) {
        const auto left = static_cast<std::size_t>(
            std::floor(position));
        const auto right = std::min(
            left + 1,
            decoded.size() - 1);
        const auto fraction = position
            - static_cast<double>(left);
        return decoded[left]
            + (decoded[right] - decoded[left])
                * fraction;
    };

    for (std::size_t frame = 0; frame < 10; ++frame)
    {
        const auto streamed = consumer->readNext(0.5);
        require(
            streamed.status
                == talas::runtime::AudioStreamReadStatus::Available,
            "prewarmed fractional-rate trace should remain available");
        requireNear(
            streamed.sample,
            fullLoadLinear(
                static_cast<double>(frame) * 0.5),
            "streamed linear conversion should equal the full-load oracle");
    }
    require(
        consumer->health().staleResponseBlocks == 0,
        "ordinary sequential block retirement must not be reported as stale generation data");
}

void testUnderrunIsExactAndPartitionInvariant()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "underrun.wav",
        48000.0,
        {{0.25, 0.5, 0.75, 1.0, -1.0, -0.5}});
    auto configuration = manualConfiguration();
    configuration.highWatermark = 1;
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        configuration);
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);

    const auto missing0 = consumer->readNext(1.0);
    const auto missing1 = consumer->readNext(1.0);
    require(
        missing0.status
                == talas::runtime::AudioStreamReadStatus::Missing
            && missing1.status
                == talas::runtime::AudioStreamReadStatus::Missing
            && missing0.sample == 0.0
            && missing1.sample == 0.0,
        "missing source frames should zero-fill without stalling the cursor");
    auto health = consumer->health();
    require(
        health.underrunEvents == 1
            && health.missingFrames == 2,
        "contiguous missing frames across separate calls should form one exact underrun event");

    (void) prepared.owner->pumpForTest();
    const auto available = consumer->readNext(1.0);
    require(
        available.status
            == talas::runtime::AudioStreamReadStatus::Available,
        "manual pump should satisfy the latest progressed cursor request");
    require(consumer->seek(4.0), "new seek should prepare another run");
    const auto missingAfterSeek = consumer->readNext(1.0);
    health = consumer->health();
    require(
        missingAfterSeek.status
                == talas::runtime::AudioStreamReadStatus::Missing
            && health.underrunEvents == 2
            && health.missingFrames == 3,
        "availability should close one underrun run and a later miss should open exactly one new event");
}

void testPreparedBackingIsImmutableAndCancellationIsTyped()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "immutable.wav";
    const auto snapshot = writeSnapshot(
        path,
        48000.0,
        {{0.25, 0.5, 0.75, 1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);

    std::string error;
    const std::vector<std::vector<double>> replacement = {
        {-1.0, -1.0, -1.0, -1.0},
    };
    require(
        talas::audio::writePcm16Wav(
            path,
            48000.0,
            replacement,
            error),
        "test should replace source path: " + error);
    auto consumer = prepared.owner->createConsumer(0.0);
    (void) prepared.owner->pumpForTest();
    requireNear(
        consumer->readNext(1.0).sample,
        8192.0 / 32767.0,
        "prepared owner should read its private immutable generation, not later external path bytes");

    int cancellationChecks = 0;
    auto cancelled = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration(),
        [&] { return ++cancellationChecks == 1; });
    require(
        !cancelled.ok()
            && cancelled.cancelled
            && cancelled.owner == nullptr,
        "stream preparation cancellation should remain typed and publish no partial owner");
}

void testCallbackConsumerOperationsDoNotAllocate()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "allocation.wav",
        48000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}});
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        manualConfiguration());
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);
    (void) prepared.owner->pumpForTest();

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto first = consumer->readNext(0.5);
    const auto sought = consumer->seek(2.0);
    const auto missing = consumer->readNext(1.0);
    countAllocations.store(false, std::memory_order_release);
    const auto allocations = allocationCount.load(
        std::memory_order_relaxed);

    require(
        first.status
                == talas::runtime::AudioStreamReadStatus::Available
            && sought
            && missing.status
                == talas::runtime::AudioStreamReadStatus::Missing,
        "allocation instrumentation fixture should exercise read, seek, and underrun publication");
    require(
        allocations == 0,
        "prepared callback consumer operations must not allocate");
}

void testRequestPressureAndInvalidRateAreBounded()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "pressure.wav",
        48000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}});
    auto configuration = manualConfiguration();
    configuration.requestCapacity = 2;
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        configuration);
    require(prepared.ok(), prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);
    const auto originalCursor = consumer->sourceFrameCursor();
    require(
        consumer->readNext(0.0).status
                == talas::runtime::AudioStreamReadStatus::InvalidIncrement
            && consumer->sourceFrameCursor() == originalCursor,
        "zero increment should fail without advancing callback state");
    require(consumer->seek(1.0), "first bounded seek should publish");
    require(consumer->seek(2.0), "second bounded seek should replace the pending epoch without blocking");
    require(
        consumer->health().requestRejections == 0
            && prepared.owner->pumpForTest() > 0
            && consumer->readNext(1.0).status
                == talas::runtime::AudioStreamReadStatus::Available,
        "latest-wins epoch mailbox should avoid queue rejection and prepare the newest seek");
}

void testPreparedConsumerReadsReverseAndExplicitLoopInterpolationWithoutAllocation()
{
    TemporaryDirectory directory;
    const std::vector<double> samples{
        0.0,
        0.0625,
        0.125,
        0.1875,
        0.25,
        0.3125,
        0.375,
        0.4375,
        0.5,
        0.5625,
        0.625,
        0.6875,
    };
    const auto snapshot = writeSnapshot(
        directory.path() / "reverse-loop.wav",
        4.0,
        {samples});
    auto configuration = manualConfiguration();
    configuration.blockFrames = 4;
    configuration.responseCapacity = 4;
    configuration.highWatermark = 4;
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        configuration);
    require(prepared.ok(), prepared.error);
    const auto full = prepared.owner->loadAllForOffline();
    require(full.ok(), "reverse differential oracle should full-load");
    auto consumer = prepared.owner->createConsumer(10.0, -1.0);
    auto loopConsumer = prepared.owner->createConsumer(7.5, -1.0);
    require(
        consumer != nullptr && loopConsumer != nullptr,
        "reverse and loop consumers should allocate during preparation");
    require(
        prepared.owner->pumpForTest() > 0,
        "reverse and loop consumers should prewarm before callback use");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    std::array<talas::runtime::AudioStreamReadResult, 11> reversed{};
    for (auto& result : reversed)
        result = consumer->readNext(-1.0);
    const auto wrapped = loopConsumer->readInterpolated(
        7.5,
        0.0,
        0.0,
        1.0);
    countAllocations.store(false, std::memory_order_release);

    require(
        allocationCount.load(std::memory_order_relaxed) == 0,
        "reverse and loop-boundary prepared reads must not allocate");
    require(
        std::ranges::all_of(
            reversed,
            [](const auto& result) {
                return result.status
                    == talas::runtime::AudioStreamReadStatus::Available;
            })
            && wrapped.status
                == talas::runtime::AudioStreamReadStatus::Available,
        "reverse and explicit loop-boundary reads should use prepared responses");
    for (std::size_t index = 0; index < reversed.size(); ++index)
    {
        requireNear(
            reversed[index].sample,
            full.data->monoSamples[10 - index],
            "multi-block reverse differential frame");
    }
    requireNear(
        wrapped.sample,
        (full.data->monoSamples[7]
         + full.data->monoSamples[0])
            * 0.5,
        "forward loop boundary should interpolate from the last loop frame to its explicit start frame");
    requireNear(
        loopConsumer->sourceFrameCursor(),
        0.0,
        "explicit read should publish its exact next logical cursor");
}

void testBackgroundWorkerPrewarmsAndTearsDownBoundedly()
{
    TemporaryDirectory directory;
    std::vector<double> samples(2048, 0.25);
    const auto snapshot = writeSnapshot(
        directory.path() / "background.wav",
        32000.0,
        {samples});
    auto configuration = manualConfiguration();
    configuration.responseCapacity = 1024;
    configuration.highWatermark = 1024;
    configuration.workerMode =
        talas::runtime::AudioStreamWorkerMode::Background;
    const auto started = std::chrono::steady_clock::now();
    {
        auto prepared = talas::runtime::prepareAudioStream(
            snapshot,
            configuration);
        require(prepared.ok(), prepared.error);
        auto consumer = prepared.owner->createConsumer(0.0);
        require(
            prepared.owner->prewarm(
                consumer,
                std::chrono::milliseconds{500}),
            "background worker should prewarm the high watermark off callback");
        require(
            consumer->health().decodedBlocks == 1024,
            "prewarm must not return before the frozen high watermark is published");
        require(
            consumer->readNext(1.0).status
                == talas::runtime::AudioStreamReadStatus::Available,
            "prewarmed background consumer should read immediately");
    }
    require(
        std::chrono::steady_clock::now() - started
            < std::chrono::seconds{2},
        "background worker teardown should join within a bounded deadline");
}

void testStreamProbeRejectsBytesPastTheRiffContainer()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "trailing.wav";
    std::string error;
    const std::vector<std::vector<double>> channels = {
        {0.25},
    };
    require(
        talas::audio::writePcm16Wav(
            path,
            48000.0,
            channels,
            error),
        "test should write trailing-byte fixture: " + error);
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::app);
        output.put(static_cast<char>(0x7f));
    }
    const auto hash =
        talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "trailing-byte fixture should hash");

    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Audio/Trailing";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = 1;
    snapshot.sampleRate = 48000.0;
    snapshot.frameCount = 1;
    snapshot.encoding = "pcm16-wav";
    const auto prepared = talas::runtime::prepareAudioStream(
        std::make_shared<const talas::core::ResourceSnapshot>(
            std::move(snapshot)),
        manualConfiguration());
    require(
        !prepared.ok()
            && prepared.error.find("RIFF size")
                != std::string::npos,
        "strict stream preparation should reject bytes beyond the declared RIFF container");
}

void testCapturedStreamPreparationUsesRetainedBytesWithoutPathReopen()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "captured-retained.wav";
    auto original = writeSnapshot(
        path,
        48000.0,
        {{0.25, -0.5, 0.75}},
        9,
        true);
    talas::core::ResourceSnapshot retained = *original;
    {
        std::ifstream input(path, std::ios::binary);
        retained.audioBytes.assign(
            std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{});
    }
    require(!retained.audioBytes.empty(),
        "captured retained-byte fixture should snapshot its WAV bytes");
    auto forged = retained;
    forged.audioBytes.clear();
    const auto forgedPreparation = talas::runtime::prepareAudioStream(
        std::make_shared<const talas::core::ResourceSnapshot>(
            std::move(forged)),
        manualConfiguration());
    require(!forgedPreparation.ok(),
        "captured provenance without exact inspected bytes must never reopen its pathname");
    std::filesystem::remove(path);

    auto prepared = talas::runtime::prepareAudioStream(
        std::make_shared<const talas::core::ResourceSnapshot>(
            std::move(retained)),
        manualConfiguration());
    require(prepared.ok(),
        "captured stream should prepare from retained bytes after the source path disappears: "
            + prepared.error);
    auto consumer = prepared.owner->createConsumer(0.0);
    require(consumer != nullptr,
        "captured retained-byte stream should create a consumer");
    require(prepared.owner->pumpForTest() >= 1,
        "captured retained-byte stream should decode its prepared window");
    const auto first = consumer->readNext(1.0);
    require(
        first.status == talas::runtime::AudioStreamReadStatus::Available,
        "captured retained-byte stream should decode without reopening its source path");
}

void testBackgroundWorkerNeverOwnsTheLastConsumerReference()
{
    TemporaryDirectory directory;
    std::vector<double> samples(8192, 0.25);
    const auto snapshot = writeSnapshot(
        directory.path() / "worker-lifetime.wav",
        48000.0,
        {samples});
    auto configuration = manualConfiguration();
    configuration.responseCapacity = 4096;
    configuration.highWatermark = 4096;
    configuration.workerMode =
        talas::runtime::AudioStreamWorkerMode::Background;
    auto prepared = talas::runtime::prepareAudioStream(
        snapshot,
        configuration);
    require(prepared.ok(), prepared.error);

    std::vector<std::shared_ptr<talas::runtime::AudioStreamConsumer>>
        consumers;
    consumers.reserve(16);
    for (std::size_t index = 0; index < 16; ++index)
    {
        auto consumer = prepared.owner->createConsumer(0.0);
        require(consumer != nullptr,
                "lifecycle fixture consumer should prepare");
        consumers.push_back(std::move(consumer));
    }
    const auto* lastConsumer = consumers.back().get();
    const auto activeDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{500};
    while (lastConsumer->health().decodedBlocks == 0
           && std::chrono::steady_clock::now()
               < activeDeadline)
    {
        std::this_thread::yield();
    }
    require(
        lastConsumer->health().decodedBlocks > 0,
        "lifecycle fixture should observe the worker inside the final consumer");
    std::weak_ptr<talas::runtime::DecodedBlockCacheOwner> owner =
        prepared.owner;
    prepared.owner.reset();
    consumers.clear();
    const auto retirementDeadline =
        std::chrono::steady_clock::now()
        + std::chrono::milliseconds{500};
    while (!owner.expired()
           && std::chrono::steady_clock::now()
               < retirementDeadline)
    {
        std::this_thread::yield();
    }
    require(
        owner.expired(),
        "consumer retirement on the control thread should release and join its owner without a worker self-join");
}

void testMalformedLayoutAndInvalidConfigurationFailClosed()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "malformed.wav";
    {
        std::ofstream output(path, std::ios::binary);
        output << "not a wav";
    }
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "malformed fixture should hash");
    talas::core::ResourceSnapshot malformed;
    malformed.id = "Audio/Malformed";
    malformed.kind = talas::core::ResourceSnapshotKind::Audio;
    malformed.generation = 1;
    malformed.contentHash = hash.contentHash;
    malformed.resolvedPath = path;
    malformed.channelCount = 1;
    malformed.sampleRate = 48000.0;
    malformed.frameCount = 1;
    malformed.encoding = "pcm16-wav";
    auto malformedPrepared = talas::runtime::prepareAudioStream(
        std::make_shared<const talas::core::ResourceSnapshot>(
            malformed),
        manualConfiguration());
    require(
        !malformedPrepared.ok()
            && malformedPrepared.error.find("RIFF/WAVE")
                != std::string::npos,
        "malformed WAV should fail before a worker owner is published");

    auto invalidConfiguration = manualConfiguration();
    invalidConfiguration.lowWatermark = 3;
    invalidConfiguration.highWatermark = 2;
    auto invalid = talas::runtime::prepareAudioStream(
        std::make_shared<const talas::core::ResourceSnapshot>(
            std::move(malformed)),
        invalidConfiguration);
    require(
        !invalid.ok()
            && invalid.error.find("watermark")
                != std::string::npos,
        "invalid bounded queue watermarks should fail before allocation");
}

void testCompilePreparesOneGraphScopedOwnerPerGeneration()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "graph.wav",
        48000.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0}},
        7);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 4;
    settings.resourceSnapshots.push_back(snapshot);
    auto compiled = talas::runtime::compileProject(
        diskPlayerCacheDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "audio graph should compile"
                : compiled.diagnostics.front().message);
    require(
        compiled.graph->preparedAudioStreamCount() == 1,
        "DiskPlayer and DiskCache references to one generation should share exactly one graph-scoped owner");
}

void testCompileRejectsChangedAudioBeforePublishingGraph()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "changed.wav";
    const auto snapshot = writeSnapshot(
        path,
        48000.0,
        {{0.0, 0.25, 0.5, 0.75}});
    std::string error;
    const std::vector<std::vector<double>> replacement = {
        {-1.0, -1.0, -1.0, -1.0},
    };
    require(
        talas::audio::writePcm16Wav(
            path,
            48000.0,
            replacement,
            error),
        "test should replace graph source: " + error);

    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 4;
    settings.resourceSnapshots.push_back(snapshot);
    auto compiled = talas::runtime::compileProject(
        diskPlayerCacheDocument(),
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        !compiled.ok()
            && !compiled.graph.has_value()
            && !compiled.diagnostics.empty()
            && compiled.diagnostics.front().nodeId == "player"
            && compiled.diagnostics.front().parameterId == "fileName"
            && compiled.diagnostics.front().message.find(
                   "changed")
                != std::string::npos,
        "changed audio bytes should fail at their first reachable resource parameter before publishing a partial graph");
}

void testOfflineDiskPlayerUsesDeterministicFullLoadAudio()
{
    TemporaryDirectory directory;
    const auto mono = writeSnapshot(
        directory.path() / "offline-mono.wav",
        4.0,
        {{0.0, 0.5, -1.0, 1.0}});
    auto compiled = compileAudioDocument(mono, 4.0);
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "offline mono audio should compile"
                : compiled.diagnostics.front().message);
    const auto first = talas::runtime::renderOffline(
        *compiled.graph,
        4);
    const auto restarted = talas::runtime::renderOffline(
        *compiled.graph,
        4);
    require(
        first.ok()
            && restarted.ok()
            && first.mono == restarted.mono
            && first.mono.size() == 4,
        "each offline session should restart from the exact immutable Audio generation");
    requireNear(first.mono[0], 0.0,
                "offline Audio frame zero should decode");
    requireNear(first.mono[1], 16384.0 / 32767.0,
                "offline Audio positive PCM16 should decode");
    requireNear(first.mono[2], -1.0,
                "offline Audio negative PCM16 should decode");
    requireNear(first.mono[3], 1.0,
                "offline Audio endpoint should decode");

    const auto stereo = writeSnapshot(
        directory.path() / "offline-stereo.wav",
        2.0,
        {
            {0.0, 1.0, 0.0, -1.0},
            {1.0, 0.0, -1.0, 0.0},
        });
    auto converted = compileAudioDocument(
        stereo,
        4.0);
    require(converted.ok(),
            converted.diagnostics.empty()
                ? "offline stereo rate conversion should compile"
                : converted.diagnostics.front().message);
    const auto convertedResult = talas::runtime::renderOffline(
        *converted.graph,
        4);
    require(
        convertedResult.ok()
            && convertedResult.mono.size() == 4,
        "offline stereo source-rate conversion should render through DiskCache passthrough");
    const auto half = 0.5;
    requireNear(convertedResult.mono[0], half,
                "stereo midpoint frame zero should render");
    requireNear(convertedResult.mono[1], half,
                "half-rate interpolation should bridge equal midpoint frames");
    requireNear(convertedResult.mono[2], half,
                "stereo midpoint frame one should render");
    requireNear(convertedResult.mono[3], 0.0,
                "half-rate interpolation should bridge opposite midpoint frames");
}

void testOfflineCapturedDiskCacheIgnoresInputResetsAndZeroFillsEof()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "captured-offline.wav",
        8000.0,
        {{0.25, -0.5, 1.0, 0.0}},
        3,
        true);
    auto compiled = compileAudioDocument(
        snapshot,
        8000.0,
        capturedDiskCacheDocument(*snapshot));
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "captured DiskCache should compile"
            : compiled.diagnostics.front().message);

    talas::runtime::OfflineRenderSession session(*compiled.graph);
    const auto first = session.render(2);
    const auto tail = session.render(4);
    require(
        first.ok() && tail.ok()
            && first.mono.size() == 2
            && tail.mono.size() == 4,
        "captured DiskCache should render across irregular offline partitions");
    requireNear(
        first.mono[0], 8192.0 / 32767.0,
        "captured DiskCache should begin at immutable frame zero");
    requireNear(
        first.mono[1], -16384.0 / 32767.0,
        "captured DiskCache should ignore its different oscillator input");
    requireNear(
        tail.mono[0], 1.0,
        "captured DiskCache should continue its immutable cursor");
    requireNear(
        tail.mono[1], 0.0,
        "captured DiskCache should decode its final frame");
    requireNear(
        tail.mono[2], 0.0,
        "captured DiskCache should zero-fill the first frame after EOF");
    requireNear(
        tail.mono[3], 0.0,
        "captured DiskCache should remain silent after EOF");

    session.reset();
    const auto restarted = session.render(2);
    require(
        restarted.ok() && restarted.mono == first.mono,
        "offline DiskCache reset should restart the pinned generation at frame zero");
}

void testCapturedDiskCacheRejectsMalformedAuthorityAndForgedPlans()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "captured-authority.wav",
        8000.0,
        {{0.25, -0.5, 1.0, 0.0}},
        5,
        true);
    auto malformedDocument = capturedDiskCacheDocument(
        *snapshot,
        std::string{talas::project::diskCacheCaptureProvenancePrefix}
            + std::string(
                talas::project::diskCacheCaptureFingerprintBytes,
                'A'));
    const auto malformed = compileAudioDocument(
        snapshot,
        8000.0,
        std::move(malformedDocument));
    require(
        !malformed.ok()
            && !malformed.graph.has_value()
            && !malformed.diagnostics.empty()
            && malformed.diagnostics.front().parameterId
                == "fileName"
            && malformed.diagnostics.front().message.find(
                   "malformed") != std::string::npos,
        "reserved malformed DiskCache provenance should fail during compile authority classification");

    auto compiled = compileAudioDocument(
        snapshot,
        8000.0,
        capturedDiskCacheDocument(*snapshot));
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "valid captured authority fixture should compile"
            : compiled.diagnostics.front().message);
    const auto isolateDiskCacheAuthority = [](
        talas::runtime::RuntimePlan& plan) {
        const auto oscillator = std::ranges::find(
            plan.steps,
            std::string_view{"talas.oscillator"},
            &talas::runtime::CompileStep::typeId);
        require(oscillator != plan.steps.end(),
            "DiskCache authority fixture should expose its carrier Oscillator");
        oscillator->offlineSupport =
            talas::core::ExecutionSupport::Subset;
        oscillator->preparedRealtimeSupport =
            talas::core::ExecutionSupport::Subset;
    };
    auto forgedPlan = compiled.graph->plan();
    isolateDiskCacheAuthority(forgedPlan);
    auto cache = std::ranges::find(
        forgedPlan.steps,
        std::string_view{"talas.diskCache"},
        &talas::runtime::CompileStep::typeId);
    require(
        cache != forgedPlan.steps.end()
            && cache->diskCachePlayback.has_value(),
        "valid compile should publish one exact DiskCache playback plan");
    ++cache->diskCachePlayback->generation;
    auto stream = talas::runtime::prepareAudioStream(snapshot);
    require(stream.ok(), stream.error);
    talas::runtime::PreparedRuntimeGraph forged{
        std::move(forgedPlan),
        compiled.graph->renderSettings(),
        {},
        {std::move(stream.owner)},
    };
    talas::runtime::OfflineRenderSession offline(forged);
    const auto offlineResult = offline.render(1);
    require(
        !offlineResult.ok(),
        "offline constructor/render must reject a hand-built DiskCache plan with forged generation authority");
    talas::runtime::RealtimeRenderSession realtime(forged);
    require(
        !realtime.ready()
            && realtime.error().find("forged or stale")
                != std::string_view::npos,
        "prepared realtime construction must reject a hand-built DiskCache plan with forged generation authority");

    auto downgradedPlan = compiled.graph->plan();
    isolateDiskCacheAuthority(downgradedPlan);
    auto downgradedCache = std::ranges::find(
        downgradedPlan.steps,
        std::string_view{"talas.diskCache"},
        &talas::runtime::CompileStep::typeId);
    downgradedCache->diskCachePlayback.reset();
    auto downgradedStream = talas::runtime::prepareAudioStream(snapshot);
    require(downgradedStream.ok(), downgradedStream.error);
    talas::runtime::PreparedRuntimeGraph downgraded{
        std::move(downgradedPlan),
        compiled.graph->renderSettings(),
        {},
        {std::move(downgradedStream.owner)},
    };
    talas::runtime::OfflineRenderSession downgradedOffline{downgraded};
    require(
        !downgradedOffline.render(1).ok(),
        "offline renderer must reject removal of captured DiskCache playback authority");
    talas::runtime::RealtimeRenderSession downgradedRealtime{downgraded};
    require(
        !downgradedRealtime.ready()
            && downgradedRealtime.error().find("forged or stale")
                != std::string_view::npos,
        "prepared realtime must reject removal of captured DiskCache playback authority");

    auto fingerprintPlan = compiled.graph->plan();
    isolateDiskCacheAuthority(fingerprintPlan);
    auto fingerprintCache = std::ranges::find(
        fingerprintPlan.steps,
        std::string_view{"talas.diskCache"},
        &talas::runtime::CompileStep::typeId);
    fingerprintCache->diskCachePlayback->sourceFingerprint.assign(
        talas::project::diskCacheCaptureFingerprintBytes, 'c');
    auto fingerprintStream = talas::runtime::prepareAudioStream(snapshot);
    require(fingerprintStream.ok(), fingerprintStream.error);
    talas::runtime::PreparedRuntimeGraph fingerprintForgery{
        std::move(fingerprintPlan),
        compiled.graph->renderSettings(),
        {},
        {std::move(fingerprintStream.owner)},
    };
    talas::runtime::RealtimeRenderSession fingerprintRealtime{
        fingerprintForgery};
    require(
        !fingerprintRealtime.ready()
            && fingerprintRealtime.error().find("forged or stale")
                != std::string_view::npos,
        "prepared realtime construction must reject a different canonical capture fingerprint");

    auto hashPlan = compiled.graph->plan();
    isolateDiskCacheAuthority(hashPlan);
    auto hashCache = std::ranges::find(
        hashPlan.steps,
        std::string_view{"talas.diskCache"},
        &talas::runtime::CompileStep::typeId);
    hashCache->diskCachePlayback->contentHash = "forged";
    auto hashSettings = compiled.graph->renderSettings();
    auto forgedSnapshot = std::make_shared<talas::core::ResourceSnapshot>(
        *snapshot);
    forgedSnapshot->contentHash = "forged";
    auto hashSnapshot = std::ranges::find_if(
        hashSettings.resourceSnapshots,
        [&](const auto& candidate) {
            return candidate != nullptr
                && candidate->id == snapshot->id;
        });
    require(
        hashSnapshot != hashSettings.resourceSnapshots.end(),
        "forged-hash fixture should retain exact resource identity");
    *hashSnapshot = std::move(forgedSnapshot);
    auto hashStream = talas::runtime::prepareAudioStream(snapshot);
    require(hashStream.ok(), hashStream.error);
    talas::runtime::PreparedRuntimeGraph hashForgery{
        std::move(hashPlan),
        std::move(hashSettings),
        {},
        {std::move(hashStream.owner)},
    };
    talas::runtime::RealtimeRenderSession hashRealtime{hashForgery};
    require(
        !hashRealtime.ready()
            && hashRealtime.error().find("forged or stale")
                != std::string_view::npos,
        "prepared realtime construction must reject matching noncanonical plan and snapshot hashes");
}

void testOfflineStreamedAudioRejectsReverseRateExplicitly()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "offline-reverse.wav",
        4.0,
        {{0.0, 0.25, 0.5, 0.75}});
    auto compiled = compileAudioDocument(
        snapshot,
        4.0,
        diskPlayerCacheDocument(
            "-1",
            "0.75 s",
            "1 s"));
    require(
        !compiled.ok()
            && !compiled.graph.has_value()
            && !compiled.diagnostics.empty()
            && compiled.diagnostics.front().parameterId
                == "rateScale"
            && compiled.diagnostics.front().message.find(
                   "reverse playback")
                != std::string::npos,
        "streamed Audio reverse rate should fail before graph publication instead of using the table compatibility behavior");
}

void testSourceFrameIncrementValidationAvoidsIntermediateOverflow()
{
    const auto preserved =
        talas::runtime::audioStreamSourceFrameIncrement(
            1.0e308,
            4.0e9,
            4.0e9);
    require(
        preserved.has_value()
            && *preserved == 1.0e308,
        "source-frame increment validation should not overflow when equal source and destination rates cancel");
    require(
        !talas::runtime::audioStreamSourceFrameIncrement(
             1.0e308,
             8.0,
             4.0)
             .has_value(),
        "source-frame increment validation should reject a final value outside finite double range");
}

void testCompileRejectsOverflowingStaticAudioRateBeforePublication()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "overflow-rate.wav",
        8.0,
        {{0.0, 0.25, 0.5, 0.75}});
    const auto compiled = compileAudioDocument(
        snapshot,
        4.0,
        diskPlayerCacheDocument(
            "1e308",
            "0 s",
            "1 s"));
    require(
        !compiled.ok()
            && !compiled.graph.has_value()
            && !compiled.diagnostics.empty()
            && compiled.diagnostics.front().parameterId
                == "rateScale"
            && compiled.diagnostics.front().message.find(
                   "source-frame increment")
                != std::string::npos,
        "overflow-producing static streamed Audio rate should fail before graph publication");
}

void testPreparedConnectedControlsIgnoreUnusedStaticFallbacks()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "connected-fallback.wav",
        8.0,
        {{0.25, 0.5, 0.75, 1.0}});
    const auto document = connectedDiskPlayerDocument(
        "1",
        "0",
        "1",
        "-1",
        "100 s",
        "-1");
    auto compiled = compileAudioDocument(
        snapshot,
        8.0,
        document);
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "connected fallback fixture should compile"
            : compiled.diagnostics.front().message);

    talas::runtime::RealtimeRenderSession session(
        *compiled.graph);
    require(
        session.ready(),
        "prepared connected controls should ignore unused static fallback values: "
            + std::string(session.error()));
    std::array<double, 1> output{};
    require(
        session.render(output),
        "prepared connected controls should render their effective values");
    requireNear(
        output[0],
        8192.0 / 32767.0,
        "connected controls should begin at their connected file position and rate");
    const auto decodedBeforeReset =
        session.streamHealth().totals.decodedBlocks;
    session.resetTransport();
    const auto resetDeadline =
        std::chrono::steady_clock::now()
        + std::chrono::milliseconds{500};
    while (session.streamHealth().totals.decodedBlocks
               <= decodedBeforeReset
           && std::chrono::steady_clock::now()
               < resetDeadline)
    {
        std::this_thread::yield();
    }
    require(
        session.streamHealth().totals.decodedBlocks
            > decodedBeforeReset,
        "connected filePosition reset should publish a new valid refill generation from the safe prepared start");
    output[0] = 0.0;
    require(
        session.render(output),
        "connected controls should render after transport reset");
    requireNear(
        output[0],
        8192.0 / 32767.0,
        "connected filePosition reset should restart from its valid connected position");
}

void testConnectedOverflowRateFailsBeforePlaybackMutation()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "connected-overflow.wav",
        8.0,
        {{0.0, 0.25, 0.75, 1.0, -1.0, -0.5, 0.5, 0.0}});
    auto document = connectedDiskPlayerDocument(
        "!Rate",
        "!Position",
        "!Trigger");
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 1;
    settings.resourceSnapshots.push_back(snapshot);
    settings.eventValues = {
        {"Rate", 1.0, talas::expr::ValueDomain::Scalar},
        {"Position", 0.0, talas::expr::ValueDomain::Scalar},
        {"Trigger", 0.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document,
        talas::core::makeBuiltinNodeRegistry(),
        std::move(settings));
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "dynamic overflow fixture should compile"
            : compiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession session(
        *compiled.graph);
    require(session.ready(), std::string(session.error()));

    std::array<double, 1> output{};
    require(
        session.render(output) && output[0] == 0.0,
        "closed connected trigger should leave playback inactive");
    const auto healthBeforeFailure =
        session.streamHealth().totals;
    require(
        session.updateEventValue(
            "Rate",
            1.0e308,
            talas::expr::ValueDomain::Scalar)
            && session.updateEventValue(
                "Position",
                0.25,
                talas::expr::ValueDomain::Scalar)
            && session.updateEventValue(
                "Trigger",
                1.0,
                talas::expr::ValueDomain::Scalar),
        "overflow callback fixture updates should publish");
    require(
        !session.render(output) && output[0] == 0.0,
        "overflow-producing connected rate should fail and zero the callback");
    const auto healthAfterFailure =
        session.streamHealth().totals;
    require(
        healthAfterFailure.staleResponseBlocks
                == healthBeforeFailure.staleResponseBlocks
            && healthAfterFailure.missingFrames
                == healthBeforeFailure.missingFrames,
        "failed connected overflow must not discard a prepared generation or enter the stream read path");

    require(
        session.updateEventValue(
            "Rate",
            1.0,
            talas::expr::ValueDomain::Scalar)
            && session.updateEventValue(
                "Trigger",
                0.0,
                talas::expr::ValueDomain::Scalar),
        "post-failure valid connected controls should publish");
    output[0] = -1.0;
    require(
        session.render(output) && output[0] == 0.0,
        "failed connected overflow must not publish a seek or leave playback active");
    require(
        session.streamHealth().totals.missingFrames
            == healthAfterFailure.missingFrames,
        "inactive follow-up after connected overflow must not enter an illicit playback read");
}

void testPreparedRealtimeDiskPlayerMatchesOfflineWithoutAllocation()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "realtime.wav",
        4.0,
        {{0.0, 0.5, -1.0, 1.0}});
    auto compiled = compileAudioDocument(snapshot, 4.0);
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "prepared Audio fixture should compile"
                : compiled.diagnostics.front().message);
    const auto offline = talas::runtime::renderOffline(
        *compiled.graph,
        4);
    require(offline.ok(), "offline conformance oracle should render");

    talas::runtime::RealtimeRenderSession session(
        *compiled.graph);
    require(
        session.ready(),
        std::string(session.error()));
    compiled.graph.reset();

    std::vector<double> realtime;
    realtime.reserve(4);
    std::array<double, 1> first{};
    std::array<double, 2> middle{};
    std::array<double, 1> last{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto renderedFirst = session.render(first);
    const auto renderedMiddle = session.render(middle);
    const auto renderedLast = session.render(last);
    countAllocations.store(false, std::memory_order_release);
    const auto allocations = allocationCount.load(
        std::memory_order_relaxed);
    realtime.insert(realtime.end(), first.begin(), first.end());
    realtime.insert(realtime.end(), middle.begin(), middle.end());
    realtime.insert(realtime.end(), last.begin(), last.end());

    require(
        renderedFirst && renderedMiddle && renderedLast,
        "prepared DiskPlayer/DiskCache should render across irregular callback partitions");
    require(
        allocations == 0,
        "prepared DiskPlayer/DiskCache callback path must not allocate");
    const auto health = session.streamHealth();
    require(
        health.consumerCount == 2
            && health.consumers.size() == 2
            && health.consumers[0].nodeId == "player"
            && health.consumers[0].structuralSourcePath == "player"
            && health.consumers[0].resourceId == "Audio/Test"
            && health.consumers[0].role
                == talas::runtime::RealtimeStreamRole::Playback
            && health.consumers[1].nodeId == "cache"
            && health.consumers[1].role
                == talas::runtime::RealtimeStreamRole::CachePrewarm
            && health.consumers[0].health.bufferCapacity > 0
            && health.totals.underrunEvents == 0
            && health.totals.missingFrames == 0
            && health.totals.decodeFailures == 0,
        "prewarmed DiskPlayer plus DiskCache should expose scoped atomic consumer telemetry");
    require(
        realtime.size() == offline.mono.size(),
        "prepared and offline traces should have equal length");
    for (std::size_t frame = 0;
         frame < realtime.size();
         ++frame)
    {
        requireNear(
            realtime[frame],
            offline.mono[frame],
            "prepared streamed trace should equal deterministic full-load output");
    }
}

// A DiskPlayer's Trigger may follow an Event Value, such as
// !KeyDown, which starts it from FilePosition when it becomes positive and
// again at each new Trigger, at once, in realtime as offline.
void testPreparedRealtimeDiskPlayerFollowsEventValues()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "events.wav",
        8.0,
        {{0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1.0}});
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        {"player", "talas.diskPlayer", {
             resourceParameter("fileName", "Audio/Test"),
             expressionParameter("rateScale", "1"),
             expressionParameter("filePosition", "0.25 s"),
             expressionParameter("duration", "100 s"),
             expressionParameter("trigger", "!Trigger"),
         }, {}},
    };
    document.graph.connections = {{"player", "audioOut", "out", "audioIn"}};
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 8.0;
    settings.maxBlockFrames = 1;
    settings.resourceSnapshots.push_back(snapshot);
    settings.eventValues = {{"Trigger", 0.0, talas::expr::ValueDomain::Scalar}};
    settings.eventValueChanges = {
        {4, "Trigger", 1.0, talas::expr::ValueDomain::Scalar},
        {6, "Trigger", 0.0, talas::expr::ValueDomain::Scalar},
        {7, "Trigger", 1.0, talas::expr::ValueDomain::Scalar},
    };
    auto compiled = talas::runtime::compileProject(
        document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "a DiskPlayer following Event Values should compile"
                : compiled.diagnostics.front().message);
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 8);
    require(offline.ok() && offline.mono.size() == 8, "the offline oracle should render");
    require(offline.mono[3] == 0.0 && offline.mono[4] != 0.0 && offline.mono[7] == offline.mono[4],
        "the DiskPlayer should start from FilePosition at each new Trigger");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    for (std::size_t frame = 0; frame < 8; ++frame)
    {
        std::array<double, 1> output{};
        require(session.render(output), "the DiskPlayer should render in realtime");
        requireNear(output[0], offline.mono[frame],
            "a realtime DiskPlayer should follow its Event Values as offline does");
    }
}

void testPreparedRealtimeCapturedDiskCacheMatchesOfflineWithoutAllocation()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "captured-realtime.wav",
        8000.0,
        {{0.25, -0.5, 1.0, 0.0}},
        4,
        true);
    auto compiled = compileAudioDocument(
        snapshot,
        8000.0,
        capturedDiskCacheDocument(*snapshot));
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "prepared captured DiskCache should compile"
            : compiled.diagnostics.front().message);

    talas::runtime::OfflineRenderSession offlineSession(*compiled.graph);
    const auto offline = offlineSession.render(6);
    require(
        offline.ok() && offline.mono.size() == 6,
        "offline captured DiskCache should provide the parity oracle");

    talas::runtime::RealtimeRenderSession realtime(*compiled.graph);
    require(realtime.ready(), std::string(realtime.error()));
    std::array<double, 1> first{};
    std::array<double, 2> middle{};
    std::array<double, 2> tail{};
    std::array<double, 1> last{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto firstOk = realtime.render(first);
    const auto middleOk = realtime.render(middle);
    const auto tailOk = realtime.render(tail);
    const auto lastOk = realtime.render(last);
    countAllocations.store(false, std::memory_order_release);
    require(
        firstOk && middleOk && tailOk && lastOk
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared captured DiskCache should render irregular callbacks without allocation: "
            + std::to_string(firstOk) + "/"
            + std::to_string(middleOk) + "/"
            + std::to_string(tailOk) + "/"
            + std::to_string(lastOk) + ", allocations="
            + std::to_string(allocationCount.load(
                std::memory_order_relaxed)));
    std::vector<double> prepared;
    prepared.insert(prepared.end(), first.begin(), first.end());
    prepared.insert(prepared.end(), middle.begin(), middle.end());
    prepared.insert(prepared.end(), tail.begin(), tail.end());
    prepared.insert(prepared.end(), last.begin(), last.end());
    require(
        prepared.size() == offline.mono.size(),
        "prepared and offline captured traces should have equal length");
    for (std::size_t frame = 0; frame < prepared.size(); ++frame)
    {
        requireNear(
            prepared[frame],
            offline.mono[frame],
            "prepared captured DiskCache should equal the immutable offline oracle");
    }

    const auto health = realtime.streamHealth();
    require(
        health.consumerCount == 1
            && health.consumers.size() == 1
            && health.consumers.front().nodeId == "cache"
            && health.consumers.front().resourceId == "Audio/Test"
            && health.consumers.front().role
                == talas::runtime::RealtimeStreamRole::Playback,
        "captured DiskCache should publish one independent playback consumer");

    realtime.resetTransport();
    std::array<double, 2> restarted{};
    require(
        realtime.render(restarted),
        "prepared captured DiskCache should render after transport reset");
    requireNear(
        restarted[0], offline.mono[0],
        "prepared DiskCache reset should return to immutable frame zero");
    requireNear(
        restarted[1], offline.mono[1],
        "prepared DiskCache reset should restart sequential playback");
}

void testPreparedCapturedDiskCacheRejectsIndependentOscillatorRollbackScope()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "captured-coexistence.wav",
        8000.0,
        {{0.25, -0.5, 1.0, 0.0}},
        4,
        true);
    auto document = capturedDiskCacheDocument(*snapshot);
    document.graph.nodes.push_back({
        "liveOscillator", "talas.oscillator", {
            expressionParameter("frequency", "1000 hz"),
            expressionParameter("amplitude", "0.25"),
        }, {}});
    document.graph.connections.push_back({
        "liveOscillator", "audioOut", "out", "rightIn"});
    auto compiled = compileAudioDocument(
        snapshot, 8000.0, std::move(document));
    require(
        compiled.ok(),
        compiled.diagnostics.empty()
            ? "captured DiskCache coexistence fixture should compile"
            : compiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(
        !session.ready()
            && session.error()
                == "Realtime Oscillator requires locally transactional DSP coexistence",
        "captured DiskCache should reject an independent Oscillator rollback scope: "
            + std::string{session.error()});
}

void testPreparedUnequalRateTailMatchesOfflineEndOfStream()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "realtime-tail.wav",
        2.0,
        {{0.0, 0.5, 1.0, -1.0}});
    auto compiled = compileAudioDocument(
        snapshot,
        4.0,
        diskPlayerCacheDocument(
            "1",
            "0 s",
            "2 s"));
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "unequal-rate tail fixture should compile"
                : compiled.diagnostics.front().message);
    const auto offline = talas::runtime::renderOffline(
        *compiled.graph,
        8);
    require(offline.ok(),
            "unequal-rate offline oracle should render");

    talas::runtime::RealtimeRenderSession session(
        *compiled.graph);
    require(session.ready(), std::string(session.error()));
    std::vector<double> prepared;
    prepared.reserve(8);
    for (std::size_t partition = 0;
         partition < 4;
         ++partition)
    {
        std::array<double, 2> block{};
        require(session.render(block),
                "unequal-rate prepared tail should render");
        prepared.insert(
            prepared.end(),
            block.begin(),
            block.end());
    }
    require(prepared.size() == offline.mono.size(),
            "unequal-rate tail traces should have equal length");
    for (std::size_t frame = 0;
         frame < prepared.size();
         ++frame)
    {
        requireNear(
            prepared[frame],
            offline.mono[frame],
            "prepared EOF boundary should equal the offline full-load oracle");
    }
}

void testPreparedRealtimeHotReplacementPinsBothGenerations()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "hot.wav";
    const auto firstSnapshot = writeSnapshot(
        path,
        4.0,
        {{0.25, 0.25, 0.25, 0.25}},
        1);
    auto firstCompiled = compileAudioDocument(
        firstSnapshot,
        4.0);
    require(firstCompiled.ok(),
            firstCompiled.diagnostics.empty()
                ? "first hot generation should compile"
                : firstCompiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession firstSession(
        *firstCompiled.graph);
    require(firstSession.ready(),
            std::string(firstSession.error()));

    const auto secondSnapshot = writeSnapshot(
        path,
        4.0,
        {{-0.5, -0.5, -0.5, -0.5}},
        2);
    auto secondCompiled = compileAudioDocument(
        secondSnapshot,
        4.0);
    require(secondCompiled.ok(),
            secondCompiled.diagnostics.empty()
                ? "second hot generation should compile"
                : secondCompiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession secondSession(
        *secondCompiled.graph);
    require(secondSession.ready(),
            std::string(secondSession.error()));
    firstCompiled.graph.reset();
    secondCompiled.graph.reset();

    std::array<double, 2> firstOutput{};
    std::array<double, 2> secondOutput{};
    require(firstSession.render(firstOutput),
            "retained first generation should render after graph release and source replacement");
    require(secondSession.render(secondOutput),
            "replacement generation should render after graph release");
    for (std::size_t frame = 0;
         frame < firstOutput.size();
         ++frame)
    {
        requireNear(
            firstOutput[frame],
            8192.0 / 32767.0,
            "first session should retain generation-one private bytes");
        requireNear(
            secondOutput[frame],
            -16384.0 / 32767.0,
            "second session should read generation-two private bytes");
    }
}

void testPreparedCapturedDiskCachePinsItsImmutableGeneration()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "captured-hot.wav";
    const auto firstSnapshot = writeSnapshot(
        path,
        8000.0,
        {{0.25, 0.25, 0.25, 0.25}},
        11,
        true);
    auto firstCompiled = compileAudioDocument(
        firstSnapshot,
        8000.0,
        capturedDiskCacheDocument(*firstSnapshot));
    require(
        firstCompiled.ok(),
        firstCompiled.diagnostics.empty()
            ? "first captured DiskCache generation should compile"
            : firstCompiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession firstSession(
        *firstCompiled.graph);
    require(firstSession.ready(), std::string(firstSession.error()));

    const auto secondSnapshot = writeSnapshot(
        path,
        8000.0,
        {{-0.5, -0.5, -0.5, -0.5}},
        12,
        true);
    auto secondCompiled = compileAudioDocument(
        secondSnapshot,
        8000.0,
        capturedDiskCacheDocument(*secondSnapshot));
    require(
        secondCompiled.ok(),
        secondCompiled.diagnostics.empty()
            ? "replacement captured DiskCache generation should compile"
            : secondCompiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession secondSession(
        *secondCompiled.graph);
    require(secondSession.ready(), std::string(secondSession.error()));
    firstCompiled.graph.reset();
    secondCompiled.graph.reset();

    std::array<double, 2> first{};
    std::array<double, 2> second{};
    require(
        firstSession.render(first)
            && secondSession.render(second),
        "both captured DiskCache generations should remain independently renderable");
    for (std::size_t frame = 0; frame < first.size(); ++frame)
    {
        requireNear(
            first[frame],
            8192.0 / 32767.0,
            "retired captured DiskCache session should retain generation-eleven private bytes");
        requireNear(
            second[frame],
            -16384.0 / 32767.0,
            "replacement captured DiskCache session should use generation-twelve private bytes");
    }
}

// With Record checked a DiskCache computes its
// Input, here a DiskPlayer of the file; the capture operation records it.
void testDiskCacheRecordModePassesItsInputOfflineAndPrepared()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "record-input.wav",
        4.0,
        {{0.0, 0.25, 0.5, 0.75}});
    auto compiled = compileAudioDocument(
        snapshot,
        4.0,
        diskPlayerCacheDocument(
            "1",
            "0 s",
            "1 s",
            true));
    require(compiled.ok(),
        "DiskCache record=true should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 4);
    require(rendered.ok() && rendered.mono.size() == 4,
        "DiskCache record=true should render offline");
    requireNear(rendered.mono[1], 8192.0 / 32767.0,
        "DiskCache record=true should play its Input offline");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(),
        "DiskCache record=true should prepare in realtime: " + std::string{session.error()});
}

void testPreparedTransportResetRepublishesTheStaticStart()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "reset.wav",
        8.0,
        {{0.0, 0.25, 0.5, 0.75, 1.0, -1.0, -0.5, 0.0}});
    auto compiled = compileAudioDocument(snapshot, 8.0);
    require(compiled.ok(),
            compiled.diagnostics.empty()
                ? "reset fixture should compile"
                : compiled.diagnostics.front().message);
    talas::runtime::RealtimeRenderSession session(
        *compiled.graph);
    require(session.ready(), std::string(session.error()));

    std::array<double, 2> first{};
    require(session.render(first),
            "reset fixture should render its first partition");
    requireNear(first[0], 0.0,
                "initial playback should begin at the static start");
    requireNear(first[1], 8192.0 / 32767.0,
                "initial playback should advance before reset");

    const auto decodedBeforeReset =
        session.streamHealth().totals.decodedBlocks;
    session.resetTransport();
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds{500};
    while (session.streamHealth().totals.decodedBlocks
               <= decodedBeforeReset
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::yield();
    }
    require(
        session.streamHealth().totals.decodedBlocks
            > decodedBeforeReset,
        "stopped-boundary reset should repopulate the new consumer generation within the bounded deadline");

    std::array<double, 2> restarted{};
    require(session.render(restarted),
            "reset fixture should render after repopulation");
    requireNear(restarted[0], 0.0,
                "reset playback should restart at the static file position");
    requireNear(restarted[1], 8192.0 / 32767.0,
                "reset playback should preserve source progression");
}

void testBridgePublishesAggregatedStreamHealth()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(
        directory.path() / "bridge.wav",
        4.0,
        {{0.0, 0.25, 0.5, 0.75}});
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::ProjectRuntimeBridge bridge(
        diskPlayerCacheDocument(),
        registry);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 2;
    settings.resourceSnapshots.push_back(snapshot);
    require(
        bridge.compileAndStage(std::move(settings)).ok(),
        "bridge stream-health fixture should stage");
    const auto stagedStatus = bridge.preparedGraphStatus();
    require(
        stagedStatus.prepared != nullptr
            && stagedStatus.prepared->stageId > 0
            && stagedStatus.awaitingActivation
            && stagedStatus.activeStageId == 0
            && !stagedStatus.deviceMappingAttached
            && stagedStatus.outputDeviceRoutes.empty(),
        "accepted prepared summary should publish before callback activation without attributing old live metrics");
    const std::array deviceRoutes = {
        talas::runtime::PreparedGraphStatusSnapshot::OutputDeviceRoute{
            3,
            false,
        },
        talas::runtime::PreparedGraphStatusSnapshot::OutputDeviceRoute{
            1,
            true,
        },
    };
    const auto routedStatus = bridge.preparedGraphStatus(
        deviceRoutes);
    require(
        routedStatus.deviceMappingAttached
            && routedStatus.outputDeviceRoutes.size()
                == routedStatus.prepared->outputs.size()
            && routedStatus.outputDeviceRoutes.front()
                   .oneBasedPhysicalChannel
                == 3
            && !routedStatus.outputDeviceRoutes.front().available,
        "bridge status should preserve the host's exact unavailable physical destination without renumbering the logical output");
    bridge.setPlaying(true);

    std::array<float, 2> output{};
    std::array<float*, 1> channels{output.data()};
    require(
        bridge.render(
            {},
            channels.data(),
            channels.size(),
            output.size()),
        "bridge stream-health fixture should activate and render");
    const auto health = bridge.streamHealth();
    const auto activeStatus = bridge.preparedGraphStatus();
    require(
        health.consumerCount == 2
            && health.stageId == activeStatus.prepared->stageId
            && health.totals.missingFrames == 0
            && health.totals.decodeFailures == 0,
        "bridge should expose the active engine's aggregated DiskPlayer and DiskCache health");
    require(
        activeStatus.prepared != nullptr
            && !activeStatus.awaitingActivation
            && activeStatus.activeStageId
                == activeStatus.prepared->stageId
            && activeStatus.streamHealth.consumers.size() == 2
            && activeStatus.streamHealth.consumers[0].nodeId
                == "player",
        "bridge status should merge live stream telemetry only after the matching prepared stage activates");
}
}

int main()
{
    testFailedBlockReadDoesNotCacheSilence();
    testDeterministicDecodeAllocationFailuresRemainTyped();
    testOfflineParentStreamReadsBeyondPrewarmWithoutWorker();
    testCallbackConsumerRetainsResponsesAcrossRollback();
    testOfflineWholeCallStreamRollback();
    testOfflineWholeCallStreamAllocationFailureIsAtomic();
    testOfflineStreamRegistrySurvivesConcurrentInspection();
    testOfflineConsumerDefersInspectionUntilCommit();
    testOfflineConsumerCheckpointOwnsWorkerAndCursorState();
    testOfflineConsumerCheckpointPreservesFreshAndTerminalBoundaries();
    testOfflineConsumerCheckpointAllocationFailureIsAtomic();
    testOfflineConsumerCheckpointSynchronizesBackgroundWorker();
    testManualStreamDecodesMonoAndStereoReduction();
    testStreamDecodesWavAndAiffSamplesOfOtherWordSizes();
    testConsumersSeekIndependentlyAndDiscardStaleGenerations();
    testPreparedRestartIsExactImmediateAndNotAnOrdinarySeek();
    testPreparedRelocationReplacesTheStaleQueueWithoutAnOrdinarySeek();
    testPreparedWindowCoversTheMaximumTransposedCallback();
    testPreparedEpochMailboxSupersedesAFullStaleRequestQueue();
    testPreparedWindowBudgetFailsBeforeAllocationAndChargesUniqueBlocks();
    testLinearSourceRateConversionMatchesFullLoadOracle();
    testUnderrunIsExactAndPartitionInvariant();
    testPreparedBackingIsImmutableAndCancellationIsTyped();
    testCallbackConsumerOperationsDoNotAllocate();
    testRequestPressureAndInvalidRateAreBounded();
    testPreparedConsumerReadsReverseAndExplicitLoopInterpolationWithoutAllocation();
    testBackgroundWorkerPrewarmsAndTearsDownBoundedly();
    testStreamProbeRejectsBytesPastTheRiffContainer();
    testCapturedStreamPreparationUsesRetainedBytesWithoutPathReopen();
    testBackgroundWorkerNeverOwnsTheLastConsumerReference();
    testMalformedLayoutAndInvalidConfigurationFailClosed();
    testCompilePreparesOneGraphScopedOwnerPerGeneration();
    testCompileRejectsChangedAudioBeforePublishingGraph();
    testOfflineDiskPlayerUsesDeterministicFullLoadAudio();
    testOfflineCapturedDiskCacheIgnoresInputResetsAndZeroFillsEof();
    testCapturedDiskCacheRejectsMalformedAuthorityAndForgedPlans();
    testOfflineStreamedAudioRejectsReverseRateExplicitly();
    testSourceFrameIncrementValidationAvoidsIntermediateOverflow();
    testCompileRejectsOverflowingStaticAudioRateBeforePublication();
    testPreparedConnectedControlsIgnoreUnusedStaticFallbacks();
    testConnectedOverflowRateFailsBeforePlaybackMutation();
    testPreparedRealtimeDiskPlayerMatchesOfflineWithoutAllocation();
    testPreparedRealtimeDiskPlayerFollowsEventValues();
    testPreparedRealtimeCapturedDiskCacheMatchesOfflineWithoutAllocation();
    testPreparedCapturedDiskCacheRejectsIndependentOscillatorRollbackScope();
    testPreparedUnequalRateTailMatchesOfflineEndOfStream();
    testPreparedRealtimeHotReplacementPinsBothGenerations();
    testPreparedCapturedDiskCachePinsItsImmutableGeneration();
    testDiskCacheRecordModePassesItsInputOfflineAndPrepared();
    testPreparedTransportResetRepublishesTheStaticStart();
    testBridgePublishesAggregatedStreamHealth();
    return 0;
}
