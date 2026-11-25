#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/RealtimeLiveSetIngress.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
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

talas::project::ProjectDocument fixture()
{
    talas::project::ProjectDocument document;
    document.timeline.musicalTime.tempo = {{0, 500000}};
    document.timeline.musicalTime.meter = {
        {0, 4, 4},
        {3840, 3, 4},
    };
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "voice";
    sound.displayName = "Voice";
    document.soundDefinitions.push_back(std::move(sound));
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "bank";
    bank.displayName = "Bank";
    bank.snapshots.push_back({"snapshot", "Snapshot", {}});
    document.snapshotLibrary.banks.push_back(std::move(bank));
    document.timeline.score.markers.push_back({
        "region",
        "Region",
        {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, 960},
        talas::project::ScoreTime{
            talas::project::ScoreTimeDomain::MusicalTicks,
            0.0,
            1920},
    });
    talas::project::ProjectLiveSet set;
    set.liveSetId = "set";
    set.displayName = "Set";
    set.cells = {
        {"sound", "Sound", 0, 0,
         {talas::project::ProjectLiveSetQuantizationKind::Immediate, 0},
         talas::project::ProjectLiveSetSoundTarget{"voice", {}}, {}},
        {"snapshot", "Snapshot", 0, 1,
         {talas::project::ProjectLiveSetQuantizationKind::NextBeat, 0},
         talas::project::ProjectLiveSetSnapshotTarget{"bank", "snapshot"}, {}},
        {"region", "Region", 0, 2,
         {talas::project::ProjectLiveSetQuantizationKind::NextBar, 0},
         talas::project::ProjectLiveSetScoreRegionTarget{"region"}, {}},
        {"stop", "Stop", 0, 3,
         {talas::project::ProjectLiveSetQuantizationKind::TickInterval, 480},
         talas::project::ProjectLiveSetStopTarget{}, {}},
    };
    document.liveSetLibrary.liveSets.push_back(std::move(set));
    return document;
}

bool prepareSound(
    void*,
    const talas::project::ProjectLiveSetSoundTarget&,
    std::shared_ptr<const talas::runtime::PreparedRuntimeGraph>& graph,
    std::string&)
{
    graph = std::make_shared<talas::runtime::PreparedRuntimeGraph>();
    return true;
}

bool prepareSnapshot(
    void*,
    const talas::project::ProjectSnapshotBank& bank,
    const talas::project::ProjectSnapshot& snapshot,
    talas::runtime::PreparedLiveSetSnapshotProgram& program,
    std::string&)
{
    program.rampFrames = bank.smoothingSeconds == 0.0 ? 0 : 1;
    program.eventValueUpdates.push_back({1, 0.5, talas::expr::ValueDomain::Scalar});
    return snapshot.snapshotId == "snapshot";
}

talas::runtime::PreparedLiveSetBinding binding()
{
    return {
        .documentAuthorityGeneration = 2,
        .stageGeneration = 3,
        .stageId = 5,
        .sampleRate = 48000,
    };
}

talas::runtime::PreparedLiveSetResult prepare(
    const talas::project::ProjectDocument& document)
{
    return talas::runtime::prepareProjectLiveSet(
        document,
        binding(),
        {nullptr, prepareSound, prepareSnapshot});
}

std::shared_ptr<const talas::runtime::PreparedRuntimeGraph> toneGraph(
    double amplitude)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "375 hz";
    talas::project::ProjectParameterExpression level;
    level.parameterId = "amplitude";
    level.expression = std::to_string(amplitude);
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"tone", "talas.oscillator", {std::move(frequency), std::move(level)}},
    };
    document.graph.connections = {
        {"tone", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto compiled = talas::runtime::compileProject(
        document, registry, std::move(settings));
    require(compiled.ok(),
            "offline/realtime parity graph should compile");
    return std::make_shared<talas::runtime::PreparedRuntimeGraph>(
        std::move(*compiled.graph));
}

std::shared_ptr<const talas::runtime::PreparedRuntimeGraph>
statefulOverlayGraph(double amplitude)
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression frequency;
    frequency.parameterId = "frequency";
    frequency.expression = "375 hz";
    talas::project::ProjectParameterExpression oscillatorLevel;
    oscillatorLevel.parameterId = "amplitude";
    oscillatorLevel.expression = std::to_string(amplitude);
    talas::project::ProjectParameterExpression gain;
    gain.parameterId = "value";
    gain.expression = "previous(!Level, 0)";
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}},
        {"vca", "talas.vca", {}},
        {"gain", "talas.constant", {std::move(gain)}},
        {"tone",
         "talas.oscillator",
         {std::move(frequency), std::move(oscillatorLevel)}},
    };
    document.graph.connections = {
        {"tone", "audioOut", "vca", "audioIn"},
        {"gain", "value", "vca", "gain"},
        {"vca", "audioOut", "out", "audioIn"},
    };
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 64;
    settings.eventValues.push_back(
        {"Level", 0.0, talas::expr::ValueDomain::Scalar});
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto compiled = talas::runtime::compileProject(
        document, registry, std::move(settings));
    require(compiled.ok(),
            "stateful overlay transfer graph should compile");
    return std::make_shared<talas::runtime::PreparedRuntimeGraph>(
        std::move(*compiled.graph));
}

struct ParityPreparationContext
{
    std::shared_ptr<const talas::runtime::PreparedRuntimeGraph> sound;
    std::shared_ptr<const talas::runtime::PreparedRuntimeGraph> overlay;
};

bool prepareParitySound(
    void* opaque,
    const talas::project::ProjectLiveSetSoundTarget&,
    std::shared_ptr<const talas::runtime::PreparedRuntimeGraph>& graph,
    std::string&)
{
    const auto* context = static_cast<const ParityPreparationContext*>(opaque);
    if (context == nullptr)
        return false;
    graph = context->sound;
    return graph != nullptr;
}

bool prepareParitySnapshot(
    void* opaque,
    const talas::project::ProjectSnapshotBank&,
    const talas::project::ProjectSnapshot& snapshot,
    talas::runtime::PreparedLiveSetSnapshotProgram& program,
    std::string&)
{
    const auto* context = static_cast<const ParityPreparationContext*>(opaque);
    if (context == nullptr)
        return false;
    if (snapshot.snapshotId == "snapshot")
    {
        program.overlayGraph = context->overlay;
        return program.overlayGraph != nullptr;
    }
    program.eventValueUpdates.push_back({
        1, 0.75, talas::expr::ValueDomain::Scalar});
    return snapshot.snapshotId == "direct";
}

struct ParityExecutionContext
{
    bool realtime = false;
    std::array<double, 64> graphOutput{};
    double eventValue = 0.0;
    double variableValue = 0.0;
    std::uint64_t transportFrame = 0;
    std::uint64_t scoreEndFrame = 0;
    bool playing = false;
};

talas::runtime::PreparedLiveSetApplicationResult applyParityGraph(
    void* opaque,
    const talas::runtime::PreparedLiveSetEffect& effect) noexcept
{
    auto* context = static_cast<ParityExecutionContext*>(opaque);
    if (context == nullptr || effect.graph == nullptr)
        return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
    try
    {
        context->graphOutput.fill(0.0);
        if (context->realtime)
        {
            talas::runtime::RealtimeRenderSession session(*effect.graph);
            if (!session.ready() || !session.render(context->graphOutput))
                return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
        }
        else
        {
            talas::runtime::OfflineRenderSession session(*effect.graph);
            const auto rendered = session.render(context->graphOutput.size());
            if (!rendered.ok()
                || rendered.mono.size() != context->graphOutput.size())
            {
                return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
            }
            std::ranges::copy(rendered.mono, context->graphOutput.begin());
        }
        context->playing = true;
        return talas::runtime::PreparedLiveSetApplicationResult::
            PendingRenderConfirmation;
    }
    catch (...)
    {
        return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
    }
}

talas::runtime::PreparedLiveSetApplicationResult applyParityDirectSnapshot(
    void* opaque,
    const talas::runtime::PreparedLiveSetEffect& effect) noexcept
{
    auto* context = static_cast<ParityExecutionContext*>(opaque);
    if (context == nullptr || effect.eventValueUpdates.size() != 1
        || effect.variableUpdates.size() > 1)
    {
        return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
    }
    context->eventValue = effect.eventValueUpdates.front().value;
    if (!effect.variableUpdates.empty())
        context->variableValue = effect.variableUpdates.front().value;
    return talas::runtime::PreparedLiveSetApplicationResult::Applied;
}

talas::runtime::PreparedLiveSetApplicationResult applyParityScoreRegion(
    void* opaque,
    const talas::runtime::PreparedLiveSetEffect& effect) noexcept
{
    auto* context = static_cast<ParityExecutionContext*>(opaque);
    if (context == nullptr || effect.scoreEndFrame <= effect.scoreStartFrame)
        return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
    context->transportFrame = effect.scoreStartFrame;
    context->scoreEndFrame = effect.scoreEndFrame;
    context->playing = true;
    return talas::runtime::PreparedLiveSetApplicationResult::Applied;
}

talas::runtime::PreparedLiveSetApplicationResult applyParityStop(
    void* opaque,
    const talas::runtime::PreparedLiveSetEffect&) noexcept
{
    auto* context = static_cast<ParityExecutionContext*>(opaque);
    if (context == nullptr)
        return talas::runtime::PreparedLiveSetApplicationResult::Rejected;
    context->playing = false;
    context->scoreEndFrame = 0;
    return talas::runtime::PreparedLiveSetApplicationResult::Applied;
}

talas::runtime::PreparedLiveSetExecutionCallbacks parityCallbacks(
    ParityExecutionContext& context)
{
    return {
        .context = &context,
        .activateSound = applyParityGraph,
        .activateOverlaySnapshot = applyParityGraph,
        .applyDirectSnapshot = applyParityDirectSnapshot,
        .seekScoreRegion = applyParityScoreRegion,
        .stop = applyParityStop,
    };
}

void testPreparationOwnsCompleteTargetRecords()
{
    const auto result = prepare(fixture());
    require(result.ok(), result.error);
    require(result.prepared->binding() == binding()
                && result.prepared->cells().size() == 4,
            "prepared Live Set should retain exact authority and every cell");
    require(result.prepared->findCell("set", "region")
                == std::optional<std::size_t>{2},
            "stable cell identity should resolve without row-index authority");
    const auto& sound = std::get<talas::runtime::PreparedLiveSetSoundProgram>(
        result.prepared->cells()[0].program);
    require(sound.graph != nullptr && sound.soundDefinitionId == "voice",
            "Sound cell should own a complete prepared graph result");
    const auto& snapshot =
        std::get<talas::runtime::PreparedLiveSetSnapshotProgram>(
            result.prepared->cells()[1].program);
    require(snapshot.eventValueUpdates.size() == 1
                && snapshot.bankId == "bank"
                && snapshot.snapshotId == "snapshot",
            "Snapshot cell should own resolved fixed transaction data");
    const auto& region = std::get<talas::runtime::PreparedLiveSetScoreRegion>(
        result.prepared->cells()[2].program);
    require(region.startFrame == 24000 && region.endFrame == 48000,
            "Score cell should own exact ranged marker frames");
}

void testOverlayTransferPreservesStatefulLiveOutput()
{
    const auto sourceGraph = statefulOverlayGraph(1.0);
    const auto overlayGraph = statefulOverlayGraph(0.5);
    talas::runtime::RealtimeRenderSession source(*sourceGraph);
    talas::runtime::RealtimeRenderSession overlay(*overlayGraph);
    require(source.ready() && overlay.ready()
                && overlay.performanceStateCompatibleWith(source),
            "stateful overlay fixture should have one compatible performance-state shape");
    std::array<double, 64> output{};
    require(source.render(output)
                && source.updateEventValue(
                    "Level", 1.0, talas::expr::ValueDomain::Scalar)
                && source.render(output)
                && source.updateEventValue(
                    "Level", 0.0, talas::expr::ValueDomain::Scalar)
                && source.render(output),
            "source should advance previous() to one exact published value");
    output.fill(0.0);
    require(overlay.transferPerformanceStateFrom(source)
                && overlay.render(output)
                && std::ranges::any_of(output, [](double sample) {
                    return std::abs(sample) > 0.1;
                }),
            "overlay transfer must preserve the copied stateful output rather than fresh-previewing reset state");
}

void testQuantizationIsExactAndPartitionIndependent()
{
    const auto result = prepare(fixture());
    require(result.ok(), result.error);
    const auto& prepared = *result.prepared;
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 0, 12345)
                == std::optional<std::uint64_t>{12345},
            "Immediate launch should retain the exact accepted frame");
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 1, 0)
                == std::optional<std::uint64_t>{24000}
                && talas::runtime::preparedLiveSetLaunchFrame(prepared, 1, 24000)
                    == std::optional<std::uint64_t>{48000},
            "NextBeat should choose the strict next beat boundary");
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 2, 25000)
                == std::optional<std::uint64_t>{96000}
                && talas::runtime::preparedLiveSetLaunchFrame(prepared, 2, 96000)
                    == std::optional<std::uint64_t>{168000},
            "NextBar should follow immutable meter changes exactly");
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 3, 12000)
                == std::optional<std::uint64_t>{24000},
            "TickInterval should use the next exact global tick grid boundary");
    const auto direct = talas::runtime::preparedLiveSetLaunchFrame(
        prepared, 2, 73555);
    const auto afterIrregularPartitions =
        talas::runtime::preparedLiveSetLaunchFrame(
            prepared, 2, 73555);
    require(direct == afterIrregularPartitions,
            "launch resolution should not depend on callback partition history");
}

void testQuantizationFollowsTheLoopedTimeline()
{
    const auto result = prepare(fixture());
    require(result.ok(), result.error);
    const auto& prepared = *result.prepared;
    // A 1.75 s loop at 48 kHz and 120 BPM: beats every 24000 frames.
    const talas::runtime::PreparedLiveSetTransportLoop loop{0, 84000};
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 1, 88800, loop)
                == std::optional<std::uint64_t>{108000},
            "NextBeat on the second pass should follow the looped playhead");
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 1, 79200, loop)
                == std::optional<std::uint64_t>{84000},
            "a beat past the loop end should wait for the loop start");
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 0, 88800, loop)
                == std::optional<std::uint64_t>{88800},
            "Immediate launch should keep the exact accepted frame");
    require(talas::runtime::preparedLiveSetLaunchFrame(
                prepared, 1, 24000, std::nullopt)
                == std::optional<std::uint64_t>{48000},
            "without a loop the absolute grid still applies");
    const talas::runtime::PreparedLiveSetTransportLoop offset{12000, 60000};
    // Playhead 50000 on the third pass; the next beat (72000) lies past the
    // loop end, so the launch waits for the loop's first beat at 24000.
    require(talas::runtime::preparedLiveSetLaunchFrame(prepared, 1, 98000, offset)
                == std::optional<std::uint64_t>{120000},
            "a loop that starts off the grid should wrap to its first beat");
}

void testPreparationAndQuantizationFailClosed()
{
    auto invalidBinding = binding();
    invalidBinding.stageGeneration = 0;
    require(!talas::runtime::prepareProjectLiveSet(
                 fixture(),
                 invalidBinding,
                 {nullptr, prepareSound, prepareSnapshot}).ok(),
            "zero authority should reject before publication");
    const auto unavailable = talas::runtime::prepareProjectLiveSet(
        fixture(),
        binding(),
        {});
    require(unavailable.ok()
                && !unavailable.prepared->cells()[0].ready()
                && !unavailable.prepared->cells()[1].ready()
                && unavailable.prepared->cells()[2].ready()
                && unavailable.prepared->cells()[3].ready(),
            "one unavailable preparation service should fail only affected cells while retaining complete ready cells");

    auto smallMeter = fixture();
    smallMeter.timeline.musicalTime.meter = {{0, 1, 64}};
    auto prepared = prepare(smallMeter);
    require(prepared.ok(), prepared.error);
    require(!talas::runtime::preparedLiveSetLaunchFrame(
                 *prepared.prepared, 3, 0).has_value(),
            "tick interval larger than the active bar should fail closed");
    require(!talas::runtime::preparedLiveSetLaunchFrame(
                 *prepared.prepared, 99, 0).has_value(),
            "unknown prepared cell index should fail closed");
}

void testOfflineAndRealtimeConsumeTheSamePreparedPrograms()
{
    auto document = fixture();
    document.snapshotLibrary.banks.front().snapshots.push_back(
        {"direct", "Direct", {}});
    document.liveSetLibrary.liveSets.front().cells.push_back({
        "direct",
        "Direct",
        0,
        4,
        {},
        talas::project::ProjectLiveSetSnapshotTarget{"bank", "direct"},
        {},
    });
    ParityPreparationContext context{
        toneGraph(0.25),
        toneGraph(0.75),
    };
    const auto result = talas::runtime::prepareProjectLiveSet(
        document,
        binding(),
        {&context, prepareParitySound, prepareParitySnapshot});
    require(result.ok(), result.error);
    talas::runtime::RealtimeLiveSetScheduler offline(8);
    talas::runtime::RealtimeLiveSetScheduler realtime(8);
    const auto populate = [](talas::runtime::RealtimeLiveSetScheduler& scheduler,
                             talas::runtime::RealtimeLiveSetSourceKind source)
    {
        constexpr std::array<std::uint64_t, 5> frames{1, 4, 8, 12, 16};
        constexpr std::array<std::uint16_t, 5> cells{0, 0, 2, 3, 4};
        for (std::size_t index = 0; index < frames.size(); ++index)
        {
            if (!scheduler.schedule({
                    .sourceKind = source,
                    .preparedCellIndex = cells[index],
                    .resolvedFrame = frames[index],
                    .documentAuthorityGeneration = 2,
                    .stageGeneration = 3,
                    .stageId = 5,
                    .transportEpoch = 7,
                    .sourceGeneration = 11,
                    .routeGeneration = 13,
                    .requestGeneration = static_cast<std::uint64_t>(index) + 1U,
                }))
            {
                return false;
            }
        }
        auto winningAtFour = talas::runtime::RealtimeLiveSetRequest{
            .sourceKind = source,
            .preparedCellIndex = 1,
            .resolvedFrame = 4,
            .documentAuthorityGeneration = 2,
            .stageGeneration = 3,
            .stageId = 5,
            .transportEpoch = 7,
            .sourceGeneration = 11,
            .routeGeneration = 13,
            .requestGeneration = 7,
        };
        if (!scheduler.schedule(winningAtFour))
        {
            return false;
        }
        return true;
    };
    require(populate(offline, talas::runtime::RealtimeLiveSetSourceKind::Offline) &&
                populate(realtime, talas::runtime::RealtimeLiveSetSourceKind::Workbench),
            "prepared parity fixture should fit both bounded schedulers");

    struct TraceRecord
    {
        talas::runtime::PreparedLiveSetEffectKind kind;
        std::uint16_t cell = 0;
        std::uint64_t frame = 0;
        talas::runtime::PreparedLiveSetExecutionState state;
        std::array<double, 64> graphOutput{};
        double eventValue = 0.0;
        std::uint64_t appliedTransportFrame = 0;
        std::uint64_t appliedScoreEndFrame = 0;
        bool appliedPlaying = false;
        bool completionRequiresRenderConfirmation = false;

        bool operator==(const TraceRecord& other) const
        {
            return kind == other.kind && cell == other.cell
                && frame == other.frame && state == other.state
                && eventValue == other.eventValue
                && appliedTransportFrame == other.appliedTransportFrame
                && appliedScoreEndFrame == other.appliedScoreEndFrame
                && appliedPlaying == other.appliedPlaying
                && completionRequiresRenderConfirmation
                    == other.completionRequiresRenderConfirmation
                && std::ranges::equal(
                    graphOutput,
                    other.graphOutput,
                    [](double left, double right) {
                        return std::abs(left - right) < 1.0e-12;
                    });
        }
    };
    const auto consume = [&](talas::runtime::RealtimeLiveSetScheduler& scheduler,
                             std::span<const std::array<std::uint64_t, 2>> partitions,
                             bool realtimeExecution)
    {
        std::vector<TraceRecord> trace;
        talas::runtime::PreparedLiveSetExecutionState state;
        ParityExecutionContext execution;
        execution.realtime = realtimeExecution;
        const auto callbacks = parityCallbacks(execution);
        for (const auto& partition : partitions)
        {
            while (const auto due = scheduler.takeNextInRange(
                       partition[0], partition[1]))
            {
                const auto effect = talas::runtime::preparedLiveSetEffect(
                    *result.prepared, due->preparedCellIndex);
                require(effect.has_value(),
                        "both schedule consumers should resolve one complete closed cell effect");
                state.transportFrame = due->resolvedFrame;
                execution.transportFrame = due->resolvedFrame;
                const auto outcome = talas::runtime::executePreparedLiveSetEffect(
                    *effect, due->requestGeneration, state, callbacks);
                require(outcome.has_value(),
                        "both schedule consumers should apply one complete closed cell effect");
                state = outcome->state;
                trace.push_back({
                    effect->kind,
                    due->preparedCellIndex,
                    due->resolvedFrame,
                    state,
                    execution.graphOutput,
                    execution.eventValue,
                    execution.transportFrame,
                    execution.scoreEndFrame,
                    execution.playing,
                    outcome->completionRequiresRenderConfirmation,
                });
            }
        }
        return trace;
    };
    constexpr std::array<std::array<std::uint64_t, 2>, 1> offlinePartition{{
        {{0, 20}},
    }};
    constexpr std::array<std::array<std::uint64_t, 2>, 6> realtimePartitions{{
        {{0, 3}},
        {{3, 5}},
        {{5, 9}},
        {{9, 13}},
        {{13, 17}},
        {{17, 20}},
    }};
    const auto offlineTrace = consume(offline, offlinePartition, false);
    const auto realtimeTrace = consume(realtime, realtimePartitions, true);
    require(offlineTrace == realtimeTrace && offlineTrace.size() == 5
                && offlineTrace[0].kind
                    == talas::runtime::PreparedLiveSetEffectKind::ActivateSound
                && offlineTrace[0].completionRequiresRenderConfirmation
                && offlineTrace[1].kind
                    == talas::runtime::PreparedLiveSetEffectKind::ActivateOverlaySnapshot
                && offlineTrace[1].completionRequiresRenderConfirmation
                && offlineTrace[2].state.scoreRegionActive
                && offlineTrace[2].state.scoreEndFrame == 48000
                && offlineTrace[2].appliedTransportFrame == 24000
                && offlineTrace[2].appliedScoreEndFrame == 48000
                && offlineTrace[3].kind
                    == talas::runtime::PreparedLiveSetEffectKind::Stop
                && !offlineTrace[3].state.playing
                && !offlineTrace[3].appliedPlaying
                && offlineTrace[4].kind
                    == talas::runtime::PreparedLiveSetEffectKind::ApplyDirectSnapshot
                && offlineTrace[4].eventValue == 0.75,
            "offline and irregular realtime consumers must execute identical Sound, Snapshot, Score, and Stop results with the same-frame winner");
}
}

int main()
{
    testPreparationOwnsCompleteTargetRecords();
    testOverlayTransferPreservesStatefulLiveOutput();
    testQuantizationIsExactAndPartitionIndependent();
    testQuantizationFollowsTheLoopedTimeline();
    testPreparationAndQuantizationFailClosed();
    testOfflineAndRealtimeConsumeTheSamePreparedPrograms();
}
