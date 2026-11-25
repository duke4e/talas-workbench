#include "talas/runtime/PreparedLiveSet.h"

#include "talas/project/ProjectDocument.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/RealtimeTransport.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <ranges>

namespace talas::runtime
{
namespace
{
[[nodiscard]] bool validBinding(
    const PreparedLiveSetBinding& binding) noexcept
{
    return binding.documentAuthorityGeneration != 0
        && binding.stageGeneration != 0
        && binding.stageId != 0
        && binding.sampleRate != 0;
}

[[nodiscard]] std::optional<std::uint64_t> checkedAdd(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return std::nullopt;
    return left + right;
}

[[nodiscard]] std::optional<std::uint64_t> checkedMultiply(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    if (left != 0
        && right > std::numeric_limits<std::uint64_t>::max() / left)
    {
        return std::nullopt;
    }
    return left * right;
}

struct MeterSegment
{
    std::uint64_t originTick = 0;
    std::uint64_t beatTicks = 0;
    std::uint64_t barTicks = 0;
};

[[nodiscard]] std::optional<MeterSegment> meterAt(
    const talas::project::ProjectMusicalTime& musicalTime,
    std::uint64_t tick) noexcept
{
    if (!talas::project::isValidProjectMusicalTime(musicalTime))
        return std::nullopt;
    const auto event = std::ranges::upper_bound(
        musicalTime.meter,
        tick,
        {},
        &talas::project::MusicalMeterEvent::tick);
    if (event == musicalTime.meter.begin())
        return std::nullopt;
    const auto& selected = *std::prev(event);
    const auto quarterTicks = checkedMultiply(
        musicalTime.ticksPerQuarter,
        4);
    if (!quarterTicks.has_value()
        || *quarterTicks % selected.denominator != 0)
    {
        return std::nullopt;
    }
    const auto beatTicks = *quarterTicks / selected.denominator;
    const auto barTicks = checkedMultiply(
        beatTicks,
        selected.numerator);
    if (!barTicks.has_value() || beatTicks == 0 || *barTicks == 0)
        return std::nullopt;
    return MeterSegment{selected.tick, beatTicks, *barTicks};
}

[[nodiscard]] std::optional<std::uint64_t> nextGridTick(
    std::uint64_t currentTick,
    std::uint64_t origin,
    std::uint64_t interval) noexcept
{
    if (interval == 0 || currentTick < origin)
        return std::nullopt;
    const auto ordinal = (currentTick - origin) / interval;
    const auto nextOrdinal = checkedAdd(ordinal, 1);
    const auto delta = nextOrdinal.has_value()
        ? checkedMultiply(*nextOrdinal, interval)
        : std::optional<std::uint64_t>{};
    return delta.has_value()
        ? checkedAdd(origin, *delta)
        : std::optional<std::uint64_t>{};
}

[[nodiscard]] PreparedLiveSetResult failure(std::string error)
{
    return {{}, std::move(error)};
}
}

const PreparedLiveSetBinding& PreparedLiveSet::binding() const noexcept
{
    return binding_;
}

const talas::project::ProjectMusicalTime&
PreparedLiveSet::musicalTime() const noexcept
{
    return musicalTime_;
}

std::span<const PreparedLiveSetCell> PreparedLiveSet::cells() const noexcept
{
    return cells_;
}

const PreparedLiveSetTrackRouting* PreparedLiveSet::trackRouting(
    std::string_view liveSetId) const noexcept
{
    const auto found = std::ranges::find(
        trackRouting_, liveSetId,
        &PreparedLiveSetTrackRouting::liveSetId);
    return found == trackRouting_.end() ? nullptr : &*found;
}

std::optional<std::size_t> PreparedLiveSet::findCell(
    std::string_view liveSetId,
    std::string_view cellId) const noexcept
{
    const auto cell = std::ranges::find_if(
        cells_,
        [&](const auto& candidate) {
            return candidate.liveSetId == liveSetId
                && candidate.cellId == cellId;
        });
    return cell == cells_.end()
        ? std::optional<std::size_t>{}
        : static_cast<std::size_t>(
            std::distance(cells_.begin(), cell));
}

PreparedLiveSetResult prepareProjectLiveSet(
    const talas::project::ProjectDocument& document,
    PreparedLiveSetBinding binding,
    PreparedLiveSetPreparationServices services)
{
    if (!validBinding(binding))
        return failure("Prepared Live Set requires exact nonzero authority");
    const auto validation =
        talas::project::validateProjectLiveSetLibrary(document);
    if (!validation.ok())
        return failure("Project Live Set library is invalid");
    try
    {
        auto prepared = std::make_shared<PreparedLiveSet>();
        prepared->binding_ = binding;
        prepared->musicalTime_ = document.timeline.musicalTime;
        std::size_t cellCount = 0;
        for (const auto& liveSet : document.liveSetLibrary.liveSets)
            cellCount += liveSet.cells.size();
        prepared->cells_.reserve(cellCount);
        prepared->trackRouting_.reserve(
            document.liveSetLibrary.liveSets.size());
        for (const auto& liveSet : document.liveSetLibrary.liveSets)
        {
            PreparedLiveSetTrackRouting routing;
            routing.liveSetId = liveSet.liveSetId;
            std::array<std::uint8_t,
                talas::project::projectLiveSetColumns> indegree{};
            for (const auto& route : liveSet.routes)
            {
                routing.destinations[route.fromColumn] |=
                    1U << route.toColumn;
                routing.hasIncoming[route.toColumn] = true;
                ++indegree[route.toColumn];
                routing.hasRoutes = true;
            }
            std::array<bool, talas::project::projectLiveSetColumns> placed{};
            for (std::size_t index = 0;
                 index < routing.renderOrder.size(); ++index)
            {
                std::size_t selected = placed.size();
                for (std::size_t column = 0;
                     column < placed.size(); ++column)
                    if (!placed[column] && indegree[column] == 0)
                    {
                        selected = column;
                        break;
                    }
                if (selected == placed.size())
                    return failure("Prepared Live Set track routing contains a cycle");
                placed[selected] = true;
                routing.renderOrder[index] =
                    static_cast<std::uint8_t>(selected);
                for (std::size_t destination = 0;
                     destination < indegree.size(); ++destination)
                    if ((routing.destinations[selected]
                         & (1U << destination)) != 0)
                        --indegree[destination];
            }
            prepared->trackRouting_.push_back(std::move(routing));
        }

        std::shared_ptr<const PreparedScoreSchedule> score;
        const auto requireScore = std::ranges::any_of(
            document.liveSetLibrary.liveSets,
            [](const auto& liveSet) {
                return std::ranges::any_of(
                    liveSet.cells,
                    [](const auto& cell) {
                        return std::holds_alternative<
                            talas::project::ProjectLiveSetScoreRegionTarget>(
                                cell.target);
                    });
            });
        if (requireScore)
        {
            const auto scoreResult = prepareProjectScore(
                document,
                preparedScoreBindingForDocument(
                    document,
                    binding.stageGeneration,
                    binding.stageGeneration,
                    binding.sampleRate,
                    binding.documentAuthorityGeneration));
            if (!scoreResult.ok())
                score.reset();
            else
                score = scoreResult.schedule;
        }

        for (const auto& liveSet : document.liveSetLibrary.liveSets)
        {
            for (const auto& cell : liveSet.cells)
            {
                PreparedLiveSetCell output;
                output.liveSetId = liveSet.liveSetId;
                output.cellId = cell.cellId;
                output.row = cell.row;
                output.column = cell.column;
                output.quantization = cell.quantization;
                if (const auto* sound = std::get_if<
                        talas::project::ProjectLiveSetSoundTarget>(
                            &cell.target))
                {
                    if (services.prepareSound == nullptr)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Sound cell preparation service is unavailable"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    PreparedLiveSetSoundProgram program;
                    program.soundDefinitionId = sound->soundDefinitionId;
                    program.sourceInstanceId = sound->sourceInstanceId;
                    std::string error;
                    auto preparedSound = false;
                    try
                    {
                        preparedSound = services.prepareSound(
                            services.context,
                            *sound,
                            program.graph,
                            error);
                    }
                    catch (const std::bad_alloc&)
                    {
                        throw;
                    }
                    catch (const std::length_error&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        error = exception.what();
                    }
                    catch (...)
                    {
                        error = "Sound cell preparation raised an unknown error";
                    }
                    if (!preparedSound || program.graph == nullptr)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            error.empty()
                                ? "Sound cell preparation failed"
                                : std::move(error)};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    output.program = std::move(program);
                }
                else if (const auto* snapshot = std::get_if<
                             talas::project::ProjectLiveSetSnapshotTarget>(
                                 &cell.target))
                {
                    if (services.prepareSnapshot == nullptr)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Snapshot cell preparation service is unavailable"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    const auto bank = std::ranges::find(
                        document.snapshotLibrary.banks,
                        snapshot->bankId,
                        &talas::project::ProjectSnapshotBank::bankId);
                    if (bank == document.snapshotLibrary.banks.end())
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Snapshot cell lost its exact bank"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    const auto value = std::ranges::find(
                        bank->snapshots,
                        snapshot->snapshotId,
                        &talas::project::ProjectSnapshot::snapshotId);
                    if (value == bank->snapshots.end())
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Snapshot cell lost its exact snapshot"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    PreparedLiveSetSnapshotProgram program;
                    program.bankId = bank->bankId;
                    program.snapshotId = value->snapshotId;
                    std::string error;
                    auto preparedSnapshot = false;
                    try
                    {
                        preparedSnapshot = services.prepareSnapshot(
                            services.context,
                            *bank,
                            *value,
                            program,
                            error);
                    }
                    catch (const std::bad_alloc&)
                    {
                        throw;
                    }
                    catch (const std::length_error&)
                    {
                        throw;
                    }
                    catch (const std::exception& exception)
                    {
                        error = exception.what();
                    }
                    catch (...)
                    {
                        error = "Snapshot cell preparation raised an unknown error";
                    }
                    if (!preparedSnapshot)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            error.empty()
                                ? "Snapshot cell preparation failed"
                                : std::move(error)};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    output.program = std::move(program);
                }
                else if (const auto* region = std::get_if<
                             talas::project::ProjectLiveSetScoreRegionTarget>(
                                 &cell.target))
                {
                    if (score == nullptr)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Score cell preparation is unavailable"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    const auto marker = std::ranges::find(
                        score->markers(),
                        region->markerId,
                        &PreparedScoreMarker::markerId);
                    if (marker == score->markers().end()
                        || !marker->endFrame.has_value()
                        || *marker->endFrame <= marker->startFrame)
                    {
                        output.program = PreparedLiveSetFailedProgram{
                            "Score cell requires an exact ranged marker"};
                        prepared->cells_.push_back(std::move(output));
                        continue;
                    }
                    output.program = PreparedLiveSetScoreRegion{
                        marker->markerId,
                        marker->startFrame,
                        *marker->endFrame,
                    };
                }
                else if (const auto* mute = std::get_if<
                             talas::project::ProjectLiveSetTrackMuteTarget>(
                                 &cell.target))
                {
                    output.program = PreparedLiveSetTrackMuteProgram{
                        mute->muted};
                }
                else
                {
                    output.program = PreparedLiveSetStopProgram{};
                }
                prepared->cells_.push_back(std::move(output));
            }
        }
        return {std::move(prepared), {}};
    }
    catch (const std::bad_alloc&)
    {
        return failure("Prepared Live Set allocation failed");
    }
    catch (const std::length_error&)
    {
        return failure("Prepared Live Set exceeded supported limits");
    }
}

std::optional<PreparedLiveSetEffect> preparedLiveSetEffect(
    const PreparedLiveSet& prepared,
    std::size_t cellIndex) noexcept
{
    if (cellIndex >= prepared.cells().size()
        || cellIndex > std::numeric_limits<std::uint16_t>::max())
    {
        return std::nullopt;
    }
    const auto& cell = prepared.cells()[cellIndex];
    if (!cell.ready())
        return std::nullopt;
    PreparedLiveSetEffect effect;
    effect.cellIndex = static_cast<std::uint16_t>(cellIndex);
    if (const auto* sound = std::get_if<PreparedLiveSetSoundProgram>(
            &cell.program))
    {
        if (sound->graph == nullptr)
            return std::nullopt;
        effect.kind = PreparedLiveSetEffectKind::ActivateSound;
        effect.graph = sound->graph.get();
        return effect;
    }
    if (const auto* snapshot = std::get_if<PreparedLiveSetSnapshotProgram>(
            &cell.program))
    {
        effect.rampFrames = snapshot->rampFrames;
        if (snapshot->overlayGraph != nullptr)
        {
            effect.kind = PreparedLiveSetEffectKind::ActivateOverlaySnapshot;
            effect.graph = snapshot->overlayGraph.get();
            return effect;
        }
        if (snapshot->eventValueUpdates.empty()
            && snapshot->variableUpdates.empty())
        {
            return std::nullopt;
        }
        effect.kind = PreparedLiveSetEffectKind::ApplyDirectSnapshot;
        effect.eventValueUpdates = snapshot->eventValueUpdates;
        effect.variableUpdates = snapshot->variableUpdates;
        return effect;
    }
    if (const auto* score = std::get_if<PreparedLiveSetScoreRegion>(
            &cell.program))
    {
        if (score->endFrame <= score->startFrame)
            return std::nullopt;
        effect.kind = PreparedLiveSetEffectKind::SeekScoreRegion;
        effect.scoreStartFrame = score->startFrame;
        effect.scoreEndFrame = score->endFrame;
        return effect;
    }
    if (const auto* mute = std::get_if<PreparedLiveSetTrackMuteProgram>(
            &cell.program))
    {
        effect.kind = PreparedLiveSetEffectKind::SetTrackMute;
        effect.column = cell.column;
        effect.muted = mute->muted;
        return effect;
    }
    if (!std::holds_alternative<PreparedLiveSetStopProgram>(cell.program))
        return std::nullopt;
    effect.kind = PreparedLiveSetEffectKind::Stop;
    return effect;
}

bool reducePreparedLiveSetEffect(
    const PreparedLiveSetEffect& effect,
    std::uint64_t requestGeneration,
    PreparedLiveSetExecutionState& state) noexcept
{
    if (requestGeneration == 0)
        return false;
    const auto cellIndexPlusOne =
        static_cast<std::uint32_t>(effect.cellIndex) + 1U;
    switch (effect.kind)
    {
    case PreparedLiveSetEffectKind::ActivateSound:
        if (effect.graph == nullptr)
            return false;
        state.playing = true;
        state.scoreRegionActive = false;
        state.scoreEndFrame = 0;
        break;
    case PreparedLiveSetEffectKind::ActivateOverlaySnapshot:
        if (effect.graph == nullptr)
            return false;
        state.scoreRegionActive = false;
        state.scoreEndFrame = 0;
        break;
    case PreparedLiveSetEffectKind::ApplyDirectSnapshot:
        if (effect.eventValueUpdates.empty()
            && effect.variableUpdates.empty())
        {
            return false;
        }
        state.scoreRegionActive = false;
        state.scoreEndFrame = 0;
        break;
    case PreparedLiveSetEffectKind::SeekScoreRegion:
        if (effect.scoreEndFrame <= effect.scoreStartFrame)
            return false;
        state.transportFrame = effect.scoreStartFrame;
        state.scoreEndFrame = effect.scoreEndFrame;
        state.playing = true;
        state.scoreRegionActive = true;
        break;
    case PreparedLiveSetEffectKind::SetTrackMute:
        return effect.column < talas::project::projectLiveSetColumns;
    case PreparedLiveSetEffectKind::Stop:
        state.activeRequestGeneration = 0;
        state.activeCellIndexPlusOne = 0;
        state.playing = false;
        state.scoreRegionActive = false;
        state.scoreEndFrame = 0;
        return true;
    }
    state.activeRequestGeneration = requestGeneration;
    state.activeCellIndexPlusOne = cellIndexPlusOne;
    return true;
}

std::optional<PreparedLiveSetExecutionOutcome>
executePreparedLiveSetEffect(
    const PreparedLiveSetEffect& effect,
    std::uint64_t requestGeneration,
    PreparedLiveSetExecutionState initialState,
    const PreparedLiveSetExecutionCallbacks& callbacks) noexcept
{
    PreparedLiveSetApplyFunction apply = nullptr;
    switch (effect.kind)
    {
    case PreparedLiveSetEffectKind::ActivateSound:
        apply = callbacks.activateSound;
        break;
    case PreparedLiveSetEffectKind::ActivateOverlaySnapshot:
        apply = callbacks.activateOverlaySnapshot;
        break;
    case PreparedLiveSetEffectKind::ApplyDirectSnapshot:
        apply = callbacks.applyDirectSnapshot;
        break;
    case PreparedLiveSetEffectKind::SeekScoreRegion:
        apply = callbacks.seekScoreRegion;
        break;
    case PreparedLiveSetEffectKind::SetTrackMute:
        apply = callbacks.setTrackMute;
        break;
    case PreparedLiveSetEffectKind::Stop:
        apply = callbacks.stop;
        break;
    }
    if (apply == nullptr)
        return std::nullopt;
    if (!reducePreparedLiveSetEffect(
            effect, requestGeneration, initialState))
    {
        return std::nullopt;
    }
    const auto application = apply(callbacks.context, effect);
    if (application == PreparedLiveSetApplicationResult::Rejected)
        return std::nullopt;
    return PreparedLiveSetExecutionOutcome{
        initialState,
        application
            == PreparedLiveSetApplicationResult::PendingRenderConfirmation,
    };
}

std::optional<std::uint64_t> preparedLiveSetLaunchFrame(
    const PreparedLiveSet& prepared,
    std::size_t cellIndex,
    std::uint64_t currentAbsoluteFrame) noexcept
{
    if (!validBinding(prepared.binding())
        || cellIndex >= prepared.cells().size())
    {
        return std::nullopt;
    }
    const auto& quantization =
        prepared.cells()[cellIndex].quantization;
    if (quantization.kind
        == talas::project::ProjectLiveSetQuantizationKind::Immediate)
    {
        return currentAbsoluteFrame;
    }
    const auto currentTick = talas::project::sampleFrameToMusicalTick(
        prepared.musicalTime(),
        currentAbsoluteFrame,
        prepared.binding().sampleRate);
    if (!currentTick.has_value())
        return std::nullopt;
    const auto meter = meterAt(prepared.musicalTime(), *currentTick);
    if (!meter.has_value())
        return std::nullopt;

    std::uint64_t origin = meter->originTick;
    std::uint64_t interval = 0;
    switch (quantization.kind)
    {
    case talas::project::ProjectLiveSetQuantizationKind::NextBeat:
        interval = meter->beatTicks;
        break;
    case talas::project::ProjectLiveSetQuantizationKind::NextBar:
        interval = meter->barTicks;
        break;
    case talas::project::ProjectLiveSetQuantizationKind::TickInterval:
        origin = 0;
        interval = quantization.tickInterval;
        if (interval == 0 || interval > meter->barTicks)
            return std::nullopt;
        break;
    case talas::project::ProjectLiveSetQuantizationKind::Immediate:
        return currentAbsoluteFrame;
    }
    const auto targetTick = nextGridTick(*currentTick, origin, interval);
    const auto targetFrame = targetTick.has_value()
        ? talas::project::musicalTickToSampleFrame(
            prepared.musicalTime(),
            *targetTick,
            prepared.binding().sampleRate)
        : std::optional<std::uint64_t>{};
    return targetFrame.has_value() && *targetFrame > currentAbsoluteFrame
        ? targetFrame
        : std::optional<std::uint64_t>{};
}

std::optional<std::uint64_t> preparedLiveSetLaunchFrame(
    const PreparedLiveSet& prepared,
    std::size_t cellIndex,
    std::uint64_t currentAbsoluteFrame,
    std::optional<PreparedLiveSetTransportLoop> loop) noexcept
{
    if (!loop.has_value() || loop->endFrame <= loop->startFrame)
        return preparedLiveSetLaunchFrame(
            prepared, cellIndex, currentAbsoluteFrame);
    const auto length = loop->endFrame - loop->startFrame;
    const auto playhead = currentAbsoluteFrame < loop->startFrame
        ? currentAbsoluteFrame
        : loop->startFrame
            + (currentAbsoluteFrame - loop->startFrame) % length;
    const auto target =
        preparedLiveSetLaunchFrame(prepared, cellIndex, playhead);
    if (!target.has_value())
        return std::nullopt;
    if (*target == playhead)
        return currentAbsoluteFrame;
    if (*target < loop->endFrame)
        return saturatingTransportFrameAdd(
            currentAbsoluteFrame, *target - playhead);
    const auto firstInLoop = loop->startFrame == 0
        ? std::optional<std::uint64_t>{0}
        : preparedLiveSetLaunchFrame(
            prepared, cellIndex, loop->startFrame - 1U);
    const auto wrappedTarget =
        firstInLoop.has_value() && *firstInLoop < loop->endFrame
        ? *firstInLoop
        : loop->startFrame;
    return saturatingTransportFrameAdd(
        currentAbsoluteFrame,
        saturatingTransportFrameAdd(
            loop->endFrame - playhead,
            wrappedTarget - loop->startFrame));
}
}
