#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/WavWriter.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
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
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
constexpr auto scalar = talas::expr::ValueDomain::Scalar;
using Parameter = talas::project::ProjectParameterExpression;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

class AllocationCounter
{
public:
    AllocationCounter()
    {
        allocationCount.store(0, std::memory_order_relaxed);
        countAllocations.store(true, std::memory_order_relaxed);
    }
    ~AllocationCounter() { countAllocations.store(false, std::memory_order_relaxed); }
    std::size_t count() const { return allocationCount.load(std::memory_order_relaxed); }
};

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
            / ("talas-time-frequency-scale-disk-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())))
    { std::filesystem::create_directories(path_); }
    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

Parameter boolean(std::string id, bool value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

Parameter resource(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

talas::project::ProjectDocument diskDocument(std::string scale = "1", std::string rate = "1")
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput"},
        {"reader", "talas.timeFrequencyScale", {
            {"frequencyScale", std::move(scale)}, {"rate", std::move(rate)}, {"gate", "1"},
            {"minInputFreq", "1 hz"}, {"maxInputFreq", "8 hz"},
            {"maxFreqScale", "4"}, {"detectors", "10"},
            boolean("fromDisk", true), resource("sample", "Audio/Scale"),
            boolean("fromMemoryWriter", false)}}};
    document.graph.connections = {{"reader", "audioOut", "out", "audioIn"}};
    return document;
}

talas::core::ResourceSnapshotPtr writeSnapshot(const std::filesystem::path& path,
    double sampleRate, const std::vector<double>& samples)
{
    std::string error;
    const std::array<std::vector<double>, 1> channels{samples};
    require(talas::audio::writePcm16Wav(path, sampleRate, channels, error),
        "PCM16 source should write: " + error);
    const auto hash = talas::project::calculateProjectAssetSha256(path);
    require(hash.ok(), "PCM16 source should hash");
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Audio/Scale";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash = hash.contentHash;
    snapshot.resolvedPath = path;
    snapshot.channelCount = 1;
    snapshot.sampleRate = sampleRate;
    snapshot.frameCount = samples.size();
    snapshot.encoding = "pcm16-wav";
    return std::make_shared<const talas::core::ResourceSnapshot>(std::move(snapshot));
}

talas::runtime::RenderSettings settingsFor(talas::core::ResourceSnapshotPtr snapshot,
    std::size_t blockFrames = 31)
{
    talas::runtime::RenderSettings settings{snapshot->sampleRate, blockFrames};
    settings.resourceSnapshots.push_back(std::move(snapshot));
    return settings;
}

talas::runtime::PreparedRuntimeGraph compile(const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings)
{
    auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    std::string errors;
    for (const auto& diagnostic : compiled.diagnostics) errors += diagnostic.message + '\n';
    require(compiled.ok(), "TimeFrequencyScale disk fixture should compile: " + errors);
    return std::move(*compiled.graph);
}

void requireNear(std::span<const double> actual, std::span<const double> expected,
    const std::string& label)
{
    require(actual.size() == expected.size(), label + " size");
    for (std::size_t index = 0; index < actual.size(); ++index)
        require(std::isfinite(actual[index]) && std::abs(actual[index] - expected[index]) <= 1.0e-10,
            label + " frame " + std::to_string(index));
}

void testDiskFileSurvivesSourceReplacementAndProjectRoundTrip()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "source.wav";
    const std::vector<double> samples{0, 8192.0 / 32767.0, -16384.0 / 32767.0, 1};
    const auto snapshot = writeSnapshot(path, 64, samples);
    auto document = diskDocument();
    document.assets.push_back({"Audio/Scale", talas::project::ProjectAssetKind::Audio,
        "source.wav", talas::project::ProjectAssetDisposition::Referenced,
        snapshot->contentHash, 1, 64, samples.size(), "pcm16-wav", 1, "repository-original"});
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto saved = talas::project::serializeProjectDocument(document, registry);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok(), "disk source document should roundtrip");
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        reopened.document, registry, directory.path() / "fixture.json");
    require(loaded.ok() && loaded.snapshots.size() == 1,
        "reopened disk source must load through the native project asset pipeline");
    const auto graph = compile(reopened.document, settingsFor(loaded.snapshots.front()));
    require(graph.preparedAudioStreamCount() == 1, "disk mode must prepare an Audio authority");
    static_cast<void>(writeSnapshot(path, 64, {-1, -1, -1, -1}));
    // Audio decodes PCM16 as the inverse of the WAV writer's 32767 scale.
    const std::vector<double> expected{0, 8192.0 / 32767.0, -16384.0 / 32767.0, 1, 0, 0, 0};
    const auto offline = talas::runtime::renderOffline(graph, expected.size());
    require(offline.ok(), "disk source must render offline after path replacement");
    requireNear(offline.mono, expected, "independent PCM16 source");
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(prepared.ready(), "disk source must prepare: " + std::string{prepared.error()});
    std::array<double, 7> actual{};
    bool ok = false;
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        ok = prepared.render(actual);
        allocations = counter.count();
    }
    require(ok && allocations == 0, "prepared disk playback must allocate nothing");
    requireNear(actual, expected, "prepared immutable PCM16 source");
}

Parameter& parameter(talas::project::ProjectDocument& document, std::string_view id)
{
    auto& reader = *std::ranges::find(document.graph.nodes, std::string_view{"reader"},
        &talas::project::ProjectNode::instanceId);
    return *std::ranges::find(reader.parameters, id, &Parameter::parameterId);
}

bool renderAndCount(talas::runtime::RealtimeRenderSession& session,
    std::span<double> output, std::size_t& allocations)
{
    AllocationCounter counter;
    const bool ok = session.render(output);
    allocations = counter.count();
    return ok;
}

void renderAll(talas::runtime::RealtimeRenderSession& session, std::span<double> output,
    std::size_t partition, std::span<const talas::runtime::EventValueChange> changes = {})
{
    for (std::size_t offset = 0; offset < output.size(); offset += partition)
    {
        const auto count = std::min(partition, output.size() - offset);
        for (const auto& change : changes)
            if (change.frame >= offset && change.frame < offset + count)
                require(session.scheduleEventValue(change.name, change.value, change.domain,
                    change.frame - offset), "disk control event should schedule");
        std::size_t allocations = 0;
        const bool ok = renderAndCount(session, output.subspan(offset, count), allocations);
        require(ok && allocations == 0, "disk callback should render without allocation: " + std::string{session.error()});
    }
}

void resetWithoutAllocation(talas::runtime::RealtimeRenderSession& session)
{
    std::size_t allocations = 0;
    {
        AllocationCounter counter;
        session.resetTransport();
        allocations = counter.count();
    }
    require(allocations == 0, "disk transport reset should allocate nothing");
}

std::pair<std::vector<double>, std::vector<double>> recording()
{
    std::vector<double> encoded(4096);
    std::vector<double> decoded(encoded.size());
    const auto pi = std::acos(-1.0);
    for (std::size_t index = 0; index < encoded.size(); ++index)
    {
        const auto tone = 0.25 * std::sin(2 * pi * 256 * index / 8192)
            + 0.125 * std::cos(2 * pi * 400 * index / 8192);
        const auto code = std::lround(tone * 32767);
        encoded[index] = static_cast<double>(code) / 32767;
        decoded[index] = static_cast<double>(code) / 32767;
    }
    return {std::move(encoded), std::move(decoded)};
}

void testDiskActiveProfilesMatchEquivalentStaticSourceAcrossPartitionsAndReset()
{
    TemporaryDirectory directory;
    const auto [encoded, decoded] = recording();
    const auto snapshot = writeSnapshot(directory.path() / "active.wav", 8192, encoded);
    for (const auto& [scale, rate] : std::array{
            std::pair{"1", "0.5"}, std::pair{"2", "1"}, std::pair{"0.5", "1"},
            std::pair{"1", "0.25"}, std::pair{"2", "0.25"}, std::pair{"0.25", "1"}})
    {
        auto document = diskDocument(scale, rate);
        parameter(document, "minInputFreq").expression = "20 hz";
        parameter(document, "maxInputFreq").expression = "1000 hz";
        const auto graph = compile(document, settingsFor(snapshot, 128));
        auto staticDocument = document;
        parameter(staticDocument, "fromDisk").boolean = false;
        talas::runtime::RenderSettings staticSettings{8192, 128};
        staticSettings.tableResources = {{"Audio/Scale", decoded}};
        const auto referenceGraph = compile(staticDocument, staticSettings);
        const auto frames = static_cast<std::size_t>(4096 / std::stod(rate)) + 2048;
        const auto expected = talas::runtime::renderOffline(referenceGraph, frames);
        const auto actual = talas::runtime::renderOffline(graph, frames);
        require(expected.ok() && actual.ok(), "active disk and table profiles must render");
        requireNear(actual.mono, expected.mono, "offline disk/static source equivalence");
        const auto& reader = *std::ranges::find(graph.plan().steps,
            std::string_view{"reader"}, &talas::runtime::CompileStep::nodeId);
        require(reader.resolvedLatencyFrames == 1279 && reader.cumulativeOutputLatencyFrames == 1279
                && std::ranges::all_of(std::span<const double>{actual.mono}.first(1279),
                    [](double value) { return value == 0; })
                && std::ranges::any_of(actual.mono, [](double value) { return std::abs(value) > 0.01; }),
            "disk profile must preserve exact active latency and produce nonzero DSP audio");
        for (const auto partition : {1u, 7u, 128u})
        {
            talas::runtime::RealtimeRenderSession session(graph);
            require(session.ready(), "active disk profile should prepare: " + std::string{session.error()});
            std::vector<double> output(frames);
            renderAll(session, output, partition);
            requireNear(output, expected.mono, "prepared disk/static active profile");
            if (partition == 7)
            {
                resetWithoutAllocation(session);
                renderAll(session, output, partition);
                requireNear(output, expected.mono, "reset disk/static active profile");
            }
        }
    }
}

void testDiskFractionalGateAndEofRestartUseIndependentSamples()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(directory.path() / "gate.wav", 64, {-1, 0, 1, 0});
    auto document = diskDocument("0.5", "0.5");
    document.graph.nodes.push_back({"gate", "talas.constant", {{"value", "!Playback"}}});
    document.graph.connections.push_back({"gate", "value", "reader", "gate"});
    auto settings = settingsFor(snapshot);
    settings.eventValues = {{"Playback", 0, scalar}};
    const std::vector<talas::runtime::EventValueChange> changes{
        {2, "Playback", 1, scalar}, {6, "Playback", 0, scalar},
        {7, "Playback", 1, scalar}, {16, "Playback", 0, scalar}, {17, "Playback", 1, scalar}};
    settings.eventValueChanges = changes;
    const std::array<double, 20> expected{
        0, 0, -1, -0.5, 0, 0.5, 0, -1, -0.5, 0, 0.5, 1, 0.5, 0, 0, 0, 0, -1, -0.5, 0};
    const auto offlineGraph = compile(document, settings);
    talas::runtime::OfflineRenderSession offline(offlineGraph);
    for (int repetition = 0; repetition < 2; ++repetition)
    {
        const auto output = offline.render(expected.size());
        require(output.ok(), "disk Gate timeline must render offline");
        requireNear(output.mono, expected, "disk Gate/EOF independent vector");
        offline.reset();
    }
    settings.eventValueChanges.clear();
    const auto graph = compile(document, settings);
    for (const auto partition : {1u, 3u, 7u, 31u})
    {
        talas::runtime::RealtimeRenderSession session(graph);
        require(session.ready(), "fractional disk profile should prepare");
        std::array<double, 20> output{};
        for (int repetition = 0; repetition < 2; ++repetition)
        {
            renderAll(session, output, partition, changes);
            requireNear(output, expected, "prepared fractional Gate/EOF vector");
            resetWithoutAllocation(session);
        }
    }
}

void testDiskActiveFailureRestoresCursorAndDspBeforeCorrectedRetry()
{
    TemporaryDirectory directory;
    const auto [encoded, decoded] = recording();
    const auto snapshot = writeSnapshot(directory.path() / "retry.wav", 8192, encoded);
    auto document = diskDocument("1", "0.5");
    parameter(document, "minInputFreq").expression = "20 hz";
    parameter(document, "maxInputFreq").expression = "1000 hz";
    document.graph.nodes.push_back({"rate", "talas.constant", {{"value", "!Rate"}}});
    document.graph.connections.push_back({"rate", "value", "reader", "rate"});
    auto settings = settingsFor(snapshot, 128);
    settings.eventValues = {{"Rate", 0.5, scalar}};
    for (const bool diskMode : {true, false})
    {
        auto sourceDocument = document;
        auto sourceSettings = settings;
        if (!diskMode)
        {
            parameter(sourceDocument, "fromDisk").boolean = false;
            sourceSettings.resourceSnapshots.clear();
            sourceSettings.tableResources = {{"Audio/Scale", decoded}};
        }
        const auto graph = compile(sourceDocument, sourceSettings);
        talas::runtime::RealtimeRenderSession candidate(graph);
        talas::runtime::RealtimeRenderSession pristine(graph);
        require(candidate.ready() && pristine.ready(), "disk retry sessions should prepare");
        std::vector<double> warmup(2040);
        for (auto* session : {&candidate, &pristine})
        {
            renderAll(*session, warmup, 128);
            require(std::ranges::any_of(warmup, [](double value) { return std::abs(value) > 0.01; }),
                "disk retry must advance real active DSP before failure");
        }
        require(candidate.scheduleEventValue("Rate", 0, scalar, 127), "late invalid rate should schedule");
        std::array<double, 128> failed{};
        failed.fill(17);
        std::size_t allocations = 0;
        require(!renderAndCount(candidate, failed, allocations) && allocations == 0
                && failed == std::array<double, 128>{},
            "late disk rate failure must clear output without allocation");
        require(candidate.updateEventValue("Rate", 0.5, scalar), "disk rate must be repairable");
        std::array<double, 128> retried{};
        std::array<double, 128> fresh{};
        const auto retryOk = renderAndCount(candidate, retried, allocations);
        require(retryOk && allocations == 0,
            "disk retry must succeed without allocation: " + std::string{candidate.error()}
                + ", allocations=" + std::to_string(allocations));
        const auto freshOk = renderAndCount(pristine, fresh, allocations);
        require(freshOk && allocations == 0,
            "fresh disk call must succeed without allocation: " + std::string{pristine.error()}
                + ", allocations=" + std::to_string(allocations));
        requireNear(retried, fresh, "disk cursor and phase-vocoder retry");
        std::vector<double> continued(2304);
        std::vector<double> freshContinued(continued.size());
        renderAll(candidate, continued, 7);
        renderAll(pristine, freshContinued, 128);
        requireNear(continued, freshContinued, "continued disk cursor and DSP history");
    }
}

void testOfflineDiskFailureRestoresCursorBeforeValidPrefixRetry()
{
    TemporaryDirectory directory;
    const auto snapshot = writeSnapshot(directory.path() / "offline-retry.wav", 64,
        {0, 8192.0 / 32767.0, 1, 0});
    auto document = diskDocument();
    document.graph.nodes.push_back({"level", "talas.level", {
        {"left", "!FailureGain"}, {"right", "1"}, boolean("noGain", false)}});
    document.graph.nodes.push_back({"mix", "talas.mixer", {{"gain1", "2"}, {"gain2", "0"}}});
    document.graph.connections = {{"reader", "audioOut", "level", "audioIn"},
        {"level", "leftOut", "mix", "in1"}, {"level", "rightOut", "mix", "in2"},
        {"mix", "audioOut", "out", "audioIn"}};
    auto settings = settingsFor(snapshot);
    settings.eventValues = {{"FailureGain", std::numeric_limits<double>::max(), scalar}};
    const auto graph = compile(document, settings);
    talas::runtime::OfflineRenderSession candidate(graph);
    talas::runtime::OfflineRenderSession pristine(graph);
    const auto failed = candidate.render(4);
    require(!failed.ok() && failed.mono.empty(), "late disk downstream overflow must discard the render");
    const auto retried = candidate.render(2);
    const auto fresh = pristine.render(2);
    const std::array<double, 2> expected{
        0, (8192.0 / 32767.0 * std::numeric_limits<double>::max()) * 2};
    require(retried.ok() && fresh.ok() && retried.mono == fresh.mono,
        "valid disk prefix retry must match a fresh session");
    requireNear(retried.mono, expected, "restored independent offline source cursor");
}

void requirePreparedSourceRejected(const talas::project::ProjectDocument& document,
    const talas::runtime::RenderSettings& settings, const std::string& label)
{
    const auto graph = compile(document, settings);
    const auto offline = talas::runtime::renderOffline(graph, 1);
    talas::runtime::RealtimeRenderSession prepared(graph);
    require(!offline.ok() && offline.mono.empty() && !prepared.ready(),
        label + " must reject both renderers after successful compilation");
}

void requireCompileRejected(const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings, const std::string& label)
{
    const auto compiled = talas::runtime::compileProject(document,
        talas::core::makeBuiltinNodeRegistry(), std::move(settings));
    require(!compiled.ok() && !compiled.diagnostics.empty(), label + " must reject compilation");
}

void testDiskSourceRejectsMissingWrongKindAndUnsupportedMedia()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "admission.wav";
    const auto snapshot = writeSnapshot(path, 64, {0, 1, -1, 0});
    const auto settings = settingsFor(snapshot);
    auto noDisk = diskDocument();
    parameter(noDisk, "fromDisk").boolean = false;
    requireCompileRejected(noDisk, settings, "Audio snapshot used as a wavetable");
    auto both = diskDocument();
    parameter(both, "fromMemoryWriter").boolean = true;
    requireCompileRejected(both, settings, "simultaneous file and live source selection");
    talas::runtime::RenderSettings missing{64, 31};
    requirePreparedSourceRejected(diskDocument(), missing, "missing disk source");
    missing.tableResources = {{"Audio/Scale", {99, 99, 99, 99}}};
    requirePreparedSourceRejected(diskDocument(), missing, "same-named table in disk mode");
    auto mismatchedRate = settings;
    mismatchedRate.sampleRate = 128;
    requireCompileRejected(diskDocument(), mismatchedRate, "different file and render sample rates");

    const auto tooLarge = writeSnapshot(directory.path() / "too-large.wav", 64,
        std::vector<double>(1'048'577, 0));
    requireCompileRejected(diskDocument(), settingsFor(tooLarge), "source beyond the decoded-frame bound");

    std::string error;
    const std::array<std::vector<double>, 2> stereo{{{0, 1, -1, 0}, {1, 0, 0, -1}}};
    require(talas::audio::writePcm16Wav(path, 64, stereo, error), "stereo rejection fixture should write");
    auto stereoSnapshot = std::make_shared<talas::core::ResourceSnapshot>(*snapshot);
    stereoSnapshot->channelCount = 2;
    stereoSnapshot->contentHash = talas::project::calculateProjectAssetSha256(path).contentHash;
    requireCompileRejected(diskDocument(), settingsFor(stereoSnapshot), "stereo disk source");

    const auto validMono = writeSnapshot(path, 64, {0, 1, -1, 0});
    {
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        require(bool(file), "unsupported WAV fixture should open");
        file.seekp(20);
        file.put(3); // IEEE float format with the existing16-bit payload is unsupported.
        require(bool(file), "unsupported WAV fixture should change its format tag");
    }
    auto unsupported = std::make_shared<talas::core::ResourceSnapshot>(*validMono);
    unsupported->contentHash = talas::project::calculateProjectAssetSha256(path).contentHash;
    requireCompileRejected(diskDocument(), settingsFor(unsupported), "non-PCM16 source bytes");
}

void testDiskSourceRetainsCompilerOwnedSnapshotAndLatencyAuthority()
{
    TemporaryDirectory directory;
    const auto [encoded, decoded] = recording();
    const auto snapshot = writeSnapshot(directory.path() / "authority.wav", 8192, encoded);
    auto document = diskDocument("2", "1");
    parameter(document, "minInputFreq").expression = "20 hz";
    parameter(document, "maxInputFreq").expression = "1000 hz";
    const auto graph = compile(document, settingsFor(snapshot, 128));
    talas::runtime::RealtimeRenderSession valid(graph);
    require(valid.ready(), "authentic disk graph must prepare before tampering checks");
    for (int mutation = 0; mutation < 5; ++mutation)
    {
        auto forged = graph;
        auto& plan = const_cast<talas::runtime::RuntimePlan&>(forged.plan());
        auto& reader = *std::ranges::find(plan.steps, std::string_view{"reader"},
            &talas::runtime::CompileStep::nodeId);
        if (mutation == 0)
            std::ranges::find(reader.parameters, std::string_view{"fromDisk"},
                &talas::runtime::ParameterValue::id)->boolean = false;
        else if (mutation < 4)
        {
            auto altered = std::make_shared<talas::core::ResourceSnapshot>(*snapshot);
            if (mutation == 1) altered->generation = 2;
            else if (mutation == 2) altered->contentHash.assign(64, 'a');
            else altered->audioBytes.push_back(1);
            auto& settings = const_cast<talas::runtime::RenderSettings&>(forged.renderSettings());
            settings.resourceSnapshots.front() = std::move(altered);
        }
        else reader.resolvedLatencyFrames = 0;
        const auto offline = talas::runtime::renderOffline(forged, 1);
        talas::runtime::RealtimeRenderSession prepared(forged);
        require(!offline.ok() && offline.mono.empty() && !prepared.ready(),
            "disk source selection, snapshot identity/bytes and native latency must remain compiler-owned: mutation="
                + std::to_string(mutation) + ", offline=" + std::to_string(offline.ok())
                + ", prepared=" + std::to_string(prepared.ready()));
    }
}
}

int main()
{
    try
    {
        testDiskFileSurvivesSourceReplacementAndProjectRoundTrip();
        testDiskActiveProfilesMatchEquivalentStaticSourceAcrossPartitionsAndReset();
        testDiskFractionalGateAndEofRestartUseIndependentSamples();
        testDiskActiveFailureRestoresCursorAndDspBeforeCorrectedRetry();
        testOfflineDiskFailureRestoresCursorBeforeValidPrefixRetry();
        testDiskSourceRejectsMissingWrongKindAndUnsupportedMedia();
        testDiskSourceRetainsCompilerOwnedSnapshotAndLatencyAuthority();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "TimeFrequencyScale disk source reference failure: " << error.what() << '\n';
        return 1;
    }
}
