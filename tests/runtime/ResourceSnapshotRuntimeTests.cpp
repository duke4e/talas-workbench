#include "talas/core/BuiltinNodes.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/runtime/ProjectRuntimeBridge.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectParameterExpression resourceParameter(
    std::string id,
    std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
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

const talas::core::NodeRegistry& registry()
{
    static const auto value = talas::core::makeBuiltinNodeRegistry();
    return value;
}

talas::project::ProjectDocument tableDrivenDocument()
{
    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        {"vca", "talas.vca", {}, {}},
        {"carrier", "talas.oscillator", {
             expressionParameter("frequency", "0.25 hz"),
             expressionParameter("amplitude", "1"),
         }, {}},
        {"table", "talas.triggeredTableRead", {
             expressionParameter("trigger", "1"),
             resourceParameter("wavetable", "Tables/Generation"),
             expressionParameter("gate", "1"),
             booleanParameter("ignoreLoops", false),
             booleanParameter("fromMemoryWriter", false),
         }, {}},
    };
    document.graph.connections = {
        {"carrier", "audioOut", "vca", "audioIn"},
        {"table", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::core::ResourceSnapshotPtr tableSnapshot(
    std::uint64_t generation,
    std::vector<double> samples,
    char hashByte = '\0')
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Generation";
    snapshot.kind = talas::core::ResourceSnapshotKind::Table;
    snapshot.generation = generation;
    snapshot.contentHash.assign(
        64,
        hashByte == '\0'
            ? (generation == 1 ? 'a' : 'b')
            : hashByte);
    snapshot.tableSamples = std::move(samples);
    return std::make_shared<const talas::core::ResourceSnapshot>(
        std::move(snapshot));
}

talas::runtime::RenderSettings settingsFor(
    talas::core::ResourceSnapshotPtr snapshot)
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.resourceSnapshots.push_back(std::move(snapshot));
    return settings;
}

talas::core::ResourceSnapshotPtr binarySnapshot(
    std::uint64_t generation,
    char hashByte)
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Generation";
    snapshot.kind =
        talas::core::ResourceSnapshotKind::Binary;
    snapshot.generation = generation;
    snapshot.contentHash.assign(64, hashByte);
    snapshot.binaryBytes = {0x01};
    return std::make_shared<
        const talas::core::ResourceSnapshot>(
            std::move(snapshot));
}

std::array<float, 4> renderBlock(
    talas::runtime::ProjectRuntimeBridge& bridge)
{
    std::array<float, 4> output{};
    std::array<float*, 1> channels{output.data()};
    require(
        bridge.render(
            {},
            channels.data(),
            channels.size(),
            output.size()),
        "prepared resource graph should render");
    return output;
}

void requireNearBlock(
    const std::array<float, 4>& actual,
    const std::array<float, 4>& expected,
    const std::string& message)
{
    for (std::size_t frame = 0; frame < actual.size(); ++frame)
    {
        require(
            std::abs(actual[frame] - expected[frame]) < 1.0e-6F,
            message + " at frame " + std::to_string(frame));
    }
}

void testHotReplacementPublishesOnlyCompleteSnapshotGenerations()
{
    const auto document = tableDrivenDocument();
    talas::runtime::ProjectRuntimeBridge candidate(document, registry());
    talas::runtime::ProjectRuntimeBridge uninterrupted(document, registry());
    talas::runtime::ProjectRuntimeBridge oldGeneration(document, registry());
    candidate.setPlaying(true);
    uninterrupted.setPlaying(true);
    oldGeneration.setPlaying(true);

    const auto generationOne = tableSnapshot(1, {1.0, 1.0, 1.0, 1.0});
    require(candidate.compileAndStage(settingsFor(generationOne)).ok(),
            "candidate generation one should stage");
    require(uninterrupted.compileAndStage(settingsFor(generationOne)).ok(),
            "reference generation one should stage");
    require(oldGeneration.compileAndStage(settingsFor(generationOne)).ok(),
            "old-generation reference should stage");
    requireNearBlock(
        renderBlock(candidate),
        renderBlock(uninterrupted),
        "first generation should activate identically");
    (void) renderBlock(oldGeneration);

    const auto sameGenerationDifferentContent =
        tableSnapshot(
            1,
            {0.25, 0.25, 0.25, 0.25},
            'b');
    const auto rejected = candidate.compileAndStage(
        settingsFor(sameGenerationDifferentContent));
    require(
        rejected.outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "same resource generation with different content identity must be rejected before staging");
    requireNearBlock(
        renderBlock(candidate),
        renderBlock(uninterrupted),
        "same-generation identity conflict should preserve active generation one");
    (void) renderBlock(oldGeneration);

    const auto generationTwo =
        tableSnapshot(2, {0.25, 0.25, 0.25, 0.25});
    require(candidate.compileAndStage(settingsFor(generationTwo)).ok(),
            "complete generation two should stage");
    require(
        candidate.compileAndStage(
            settingsFor(tableSnapshot(
                3,
                {0.25, 0.25, 0.25, 0.25},
                'b')))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "same accepted content must not consume another resource generation");
    require(
        candidate.compileAndStage(
            settingsFor(tableSnapshot(
                1,
                {1.0, 1.0, 1.0, 1.0},
                'a')))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "an accepted resource generation must never move backwards");
    require(uninterrupted.compileAndStage(settingsFor(generationTwo)).ok(),
            "generation-two reference should stage");
    const auto replaced = renderBlock(candidate);
    const auto replacementReference = renderBlock(uninterrupted);
    const auto stillOld = renderBlock(oldGeneration);
    requireNearBlock(
        replaced,
        replacementReference,
        "generation two should become visible only after accepted staging");
    require(
        std::abs(replaced[1]) < std::abs(stillOld[1]) * 0.5F,
        "accepted generation two should replace generation-one table values");
}

void testInvalidResourceSnapshotKindIsRejected()
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Generation";
    snapshot.kind = static_cast<
        talas::core::ResourceSnapshotKind>(255);
    snapshot.generation = 1;
    snapshot.contentHash.assign(64, 'a');
    snapshot.binaryBytes = {0x01};

    auto compiled = talas::runtime::compileProject(
        tableDrivenDocument(),
        registry(),
        settingsFor(
            std::make_shared<
                const talas::core::ResourceSnapshot>(
                    std::move(snapshot))));
    require(
        !compiled.ok()
            && !compiled.diagnostics.empty()
            && compiled.diagnostics.front().message.find(
                   "kind is invalid")
                != std::string::npos,
        "invalid public resource snapshot enum values must not materialize as binary resources");
}

void testInvalidAudioResourceSnapshotShapesAreRejected()
{
    talas::core::ResourceSnapshot snapshot;
    snapshot.id = "Tables/Generation";
    snapshot.kind = talas::core::ResourceSnapshotKind::Audio;
    snapshot.generation = 1;
    snapshot.contentHash.assign(64, 'a');
    snapshot.channelCount = 3;
    snapshot.sampleRate = 48000.0;
    snapshot.frameCount = 4;
    snapshot.encoding = "pcm16-wav";

    auto compiled = talas::runtime::compileProject(
        tableDrivenDocument(),
        registry(),
        settingsFor(
            std::make_shared<
                const talas::core::ResourceSnapshot>(
                    std::move(snapshot))));
    require(
        !compiled.ok()
            && !compiled.diagnostics.empty()
            && compiled.diagnostics.front().message.find(
                   "Audio resource snapshot")
                != std::string::npos,
        "invalid public Audio snapshot media shapes must fail closed during materialization");
}

void testResourceIdentityHistoryPersistsUntilDocumentReset()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        tableDrivenDocument(),
        registry());
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                2,
                {0.25, 0.25, 0.25, 0.25},
                'b')))
            .ok(),
        "generation two should establish the resource identity ledger");
    const auto accepted = bridge.acceptedResourceIdentity(
        "Tables/Generation");
    require(
        accepted.has_value()
            && accepted->kind
                == talas::core::ResourceSnapshotKind::Table
            && accepted->generation == 2
            && accepted->contentHash
                == std::string(64, 'b'),
        "bridge should expose the latest fully accepted resource identity");

    bridge.stopTransport();
    bridge.clearAfterAudioStopped();
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                1,
                {1.0, 1.0, 1.0, 1.0},
                'a')))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "transport stop and graph clear must not erase monotonic resource history");
    require(
        bridge.compileAndStage(settingsFor(
            binarySnapshot(2, 'b')))
                .outcome
            == talas::runtime::ProjectStageOutcome::ResourceIdentityConflict,
        "resource kind must remain immutable within one document identity");

    bridge.resetResourceIdentityHistory();
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                1,
                {1.0, 1.0, 1.0, 1.0},
                'a')))
            .ok(),
        "explicit document replacement reset should admit the new document's generation one");
}

void testRejectedStageDoesNotAdvanceResourceIdentityHistory()
{
    talas::runtime::ProjectRuntimeBridge bridge(
        tableDrivenDocument(),
        registry());
    bridge.setPlaying(true);
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                1,
                {1.0, 1.0, 1.0, 1.0},
                'a')))
            .ok(),
        "generation one should occupy the pending graph slot");
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                2,
                {0.5, 0.5, 0.5, 0.5},
                'b')))
                .outcome
            == talas::runtime::ProjectStageOutcome::StageRejected,
        "second prepared graph should be rejected while the pending slot is occupied");

    (void) renderBlock(bridge);
    require(
        bridge.compileAndStage(settingsFor(
            tableSnapshot(
                2,
                {0.25, 0.25, 0.25, 0.25},
                'c')))
            .ok(),
        "a rejected stage must not reserve or partially advance generation two in the ledger");
}

void testPreparedSessionPinsSnapshotLifetimeDuringRenderThreadUse()
{
    auto snapshot = tableSnapshot(5, {1.0, 0.5, 0.25, 0.0});
    std::weak_ptr<const talas::core::ResourceSnapshot> lifetime = snapshot;
    auto compiled = talas::runtime::compileProject(
        tableDrivenDocument(),
        registry(),
        settingsFor(snapshot));
    require(compiled.ok(), "snapshot lifetime fixture should compile");
    require(
        compiled.graph->resourceSnapshots().size() == 1
            && compiled.graph->resourceSnapshots().front() == snapshot,
        "prepared graph should own the exact immutable snapshot");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), std::string(session.error()));
    snapshot.reset();
    compiled.graph.reset();
    require(!lifetime.expired(),
            "prepared session should retain its resource snapshot after graph release");

    std::atomic<bool> started = false;
    std::atomic<bool> release = false;
    std::atomic<bool> rendered = true;
    std::thread audioThread(
        [session = std::move(session), &started, &release, &rendered]() mutable {
            std::array<double, 4> output{};
            started.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire))
            {
                if (!session.render(output))
                {
                    rendered.store(false, std::memory_order_release);
                    break;
                }
            }
        });
    while (!started.load(std::memory_order_acquire))
    {
    }
    require(!lifetime.expired(),
            "render thread should never observe a dangling resource snapshot");
    release.store(true, std::memory_order_release);
    audioThread.join();
    require(rendered.load(std::memory_order_acquire),
            "render thread should retain a valid immutable snapshot");
    require(lifetime.expired(),
            "snapshot should be reclaimed after the last prepared owner exits");
}

void testLegacyRenderResourcesBecomeCompatibilitySnapshots()
{
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 4.0;
    settings.maxBlockFrames = 4;
    settings.tableResources.push_back({
        "Tables/Generation",
        {1.0, 0.5, 0.25, 0.0},
    });
    auto compiled = talas::runtime::compileProject(
        tableDrivenDocument(),
        registry(),
        std::move(settings));
    require(compiled.ok(),
            "legacy table resource should remain source compatible");
    const auto snapshots =
        compiled.graph->resourceSnapshots();
    require(
        snapshots.size() == 1
            && snapshots.front() != nullptr
            && snapshots.front()->id
                == "Tables/Generation"
            && snapshots.front()->kind
                == talas::core::ResourceSnapshotKind::Table
            && snapshots.front()->generation == 0
            && snapshots.front()->contentHash.empty()
            && snapshots.front()->tableSamples
                == std::vector<double>({1.0, 0.5, 0.25, 0.0}),
        "legacy compatibility input should be pinned as an immutable generation-zero snapshot");
}
}

int main()
{
    testHotReplacementPublishesOnlyCompleteSnapshotGenerations();
    testInvalidResourceSnapshotKindIsRejected();
    testInvalidAudioResourceSnapshotShapesAreRejected();
    testResourceIdentityHistoryPersistsUntilDocumentReset();
    testRejectedStageDoesNotAdvanceResourceIdentityHistory();
    testPreparedSessionPinsSnapshotLifetimeDuringRenderThreadUse();
    testLegacyRenderResourcesBecomeCompatibilitySnapshots();
    return 0;
}
