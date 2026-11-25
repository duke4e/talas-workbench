#include "talas/runtime/RealtimeAudioEngine.h"

#include "talas/runtime/PreparedLiveSet.h"
#include "talas/runtime/PreparedLiveTools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace talas::runtime
{
class PreparedLiveSetRuntime
{
  public:
    explicit PreparedLiveSetRuntime(const PreparedLiveSet& prepared,
                                    const RealtimeAudioEngine& base)
        : enginesByCell_(prepared.cells().size()),
          transfersPerformanceState_(prepared.cells().size(), 0)
    {
        ready_ = true;
        for (std::size_t index = 0; index < prepared.cells().size(); ++index)
        {
            const auto& program = prepared.cells()[index].program;
            std::shared_ptr<const PreparedRuntimeGraph> graph;
            if (const auto* sound = std::get_if<PreparedLiveSetSoundProgram>(&program))
            {
                graph = sound->graph;
            }
            else if (const auto* snapshot = std::get_if<PreparedLiveSetSnapshotProgram>(&program))
            {
                graph = snapshot->overlayGraph;
                transfersPerformanceState_[index] = graph != nullptr ? 1U : 0U;
            }
            if (graph == nullptr)
                continue;
            auto engine = std::make_unique<RealtimeAudioEngine>(*graph);
            if (!engine->ready())
            {
                ready_ = false;
                return;
            }
            if (transfersPerformanceState_[index] != 0
                && !engine->performanceStateCompatibleWith(base))
            {
                ready_ = false;
                return;
            }
            maximumOutputChannels_ = std::max(maximumOutputChannels_, engine->outputChannelCount());
            enginesByCell_[index] = std::move(engine);
        }
    }

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] std::size_t cellCount() const noexcept { return enginesByCell_.size(); }

    void bindAuthority(std::uint64_t stageId, std::uint64_t snapshotGeneration) noexcept
    {
        for (auto& engine : enginesByCell_)
        {
            if (engine == nullptr)
                continue;
            engine->setStageId(stageId);
            engine->setSnapshotGeneration(snapshotGeneration);
        }
    }

    [[nodiscard]] RealtimeAudioEngine* engineAt(std::size_t cellIndex) noexcept
    {
        return cellIndex < enginesByCell_.size() ? enginesByCell_[cellIndex].get() : nullptr;
    }

    [[nodiscard]] bool transfersPerformanceState(std::size_t cellIndex) const noexcept
    {
        return cellIndex < transfersPerformanceState_.size()
            && transfersPerformanceState_[cellIndex] != 0;
    }

    [[nodiscard]] std::size_t maximumOutputChannels() const noexcept
    {
        return maximumOutputChannels_;
    }

  private:
    std::vector<std::unique_ptr<RealtimeAudioEngine>> enginesByCell_;
    std::vector<std::uint8_t> transfersPerformanceState_;
    std::size_t maximumOutputChannels_ = 0;
    bool ready_ = false;
};
namespace
{
void clearOutputChannels(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept
{
    if (outputChannels == nullptr)
        return;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
        if (outputChannels[channel] != nullptr)
            std::fill_n(outputChannels[channel], frameCount, 0.0F);
}
}

RealtimeAudioEngine::RealtimeAudioEngine(const PreparedRuntimeGraph& graph)
    : session_(graph)
    , rootEventValueBindingCount_(
          graph.renderSettings().eventValues.size())
    , scratchChannels_(
          session_.outputChannelCount(),
          std::vector<double>(session_.maxBlockFrames(), 0.0))
    , scratchChannelPointers_(session_.outputChannelCount(), nullptr)
{
    for (std::size_t channel = 0; channel < scratchChannels_.size(); ++channel)
        scratchChannelPointers_[channel] = scratchChannels_[channel].data();
    for (std::size_t tap = 0; tap < session_.diskRecorderTapCount(); ++tap)
    {
        const auto* plan = session_.diskRecorderTap(tap);
        auto& channels = diskRecorderTapScratch_.emplace_back(
            plan->channelBufferIndices.size(), std::vector<float>(session_.maxBlockFrames(), 0.0F));
        auto& pointers = diskRecorderTapPointers_.emplace_back();
        for (auto& channel : channels)
            pointers.push_back(channel.data());
    }
}

std::size_t RealtimeAudioEngine::diskRecorderTapCount() const noexcept
{
    return diskRecorderTapPointers_.size();
}

std::size_t RealtimeAudioEngine::diskRecorderTapFrames() const noexcept
{
    return diskRecorderTapFrames_;
}

std::span<float* const> RealtimeAudioEngine::diskRecorderTapChannels(std::size_t tap) const noexcept
{
    return tap < diskRecorderTapPointers_.size() ? std::span<float* const>{diskRecorderTapPointers_[tap]}
                                                 : std::span<float* const>{};
}

RealtimeAudioEngine::~RealtimeAudioEngine() = default;

std::unique_ptr<RealtimeAudioEngine::PreparedTransportRelocation>
RealtimeAudioEngine::prepareTransportRelocation(
    std::uint64_t frame, std::uint64_t& remaining, std::string& error) const
{
    auto budget = remaining;
    auto result = std::make_unique<PreparedTransportRelocation>();
    result->root = session_.prepareTransportRelocation(frame, budget, error);
    if (!result->root) return {};
    if (preparedLiveSetRuntime_)
    {
        result->cells.resize(preparedLiveSetRuntime_->cellCount());
        for (std::size_t index = 0; index < result->cells.size(); ++index)
            if (const auto* cell = preparedLiveSetRuntime_->engineAt(index))
            {
                result->cells[index] = cell->prepareTransportRelocation(frame, budget, error);
                if (!result->cells[index]) return {};
            }
    }
    remaining = budget;
    return result;
}
bool RealtimeAudioEngine::canInstallTransportRelocation(const PreparedTransportRelocation& candidate) const noexcept
{
    if (!candidate.root || !session_.canInstallTransportRelocation(*candidate.root)
        || candidate.cells.size() != (preparedLiveSetRuntime_ ? preparedLiveSetRuntime_->cellCount() : 0)) return false;
    for (std::size_t index = 0; index < candidate.cells.size(); ++index)
    {
        const auto* cell = preparedLiveSetRuntime_->engineAt(index);
        if (bool(cell) != bool(candidate.cells[index])
            || (cell && !cell->canInstallTransportRelocation(*candidate.cells[index]))) return false;
    }
    return true;
}
bool RealtimeAudioEngine::installTransportRelocation(PreparedTransportRelocation& candidate) noexcept
{
    if (!canInstallTransportRelocation(candidate)) return false;
    if (!session_.installTransportRelocation(*candidate.root)) return false;
    for (std::size_t index = 0; index < candidate.cells.size(); ++index)
        if (auto* cell = preparedLiveSetRuntime_->engineAt(index))
            if (!cell->installTransportRelocation(*candidate.cells[index])) return false;
    return true;
}

bool RealtimeAudioEngine::ready() const noexcept
{
    return session_.ready();
}

std::string_view RealtimeAudioEngine::error() const noexcept
{
    return session_.error();
}

std::size_t RealtimeAudioEngine::maxBlockFrames() const noexcept
{
    return session_.maxBlockFrames();
}

std::size_t RealtimeAudioEngine::outputChannelCount() const noexcept
{
    std::size_t channels = 0;
    for (const auto& slot : activeLiveSetSoundByColumn_)
    {
        if (const auto* sound = slot.load(std::memory_order_acquire))
            channels = std::max(channels, sound->outputChannelCount());
    }
    if (channels != 0)
        return channels;
    if (const auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return live->outputChannelCount();
    }
    return session_.outputChannelCount();
}

double RealtimeAudioEngine::sampleRate() const noexcept
{
    return session_.sampleRate();
}

std::shared_ptr<const PreparedScoreSchedule>
RealtimeAudioEngine::preparedScoreSchedule() const noexcept
{
    return session_.preparedScoreSchedule();
}

bool RealtimeAudioEngine::setPreparedLiveSet(std::shared_ptr<const PreparedLiveSet> prepared)
{
    for (std::size_t column = 0;
         column < activeLiveSetSoundByColumn_.size();
         ++column)
    {
        activeLiveSetSoundByColumn_[column].store(
            nullptr, std::memory_order_release);
        activeLiveSetSoundCellByColumn_[column].store(
            0, std::memory_order_release);
        liveSetSoundMutedByColumn_[column].store(
            false, std::memory_order_release);
    }
    if (prepared == nullptr)
    {
        activeLiveSetEngine_.store(nullptr, std::memory_order_release);
        liveSetControlEngine_.store(nullptr, std::memory_order_release);
        liveSetTransitionFrom_ = nullptr;
        liveSetTransitionTo_ = nullptr;
        liveSetTransitionCellIndexPlusOne_ = 0;
        liveSetTransitionFrames_ = 0;
        liveSetTransitionCompletedFrames_ = 0;
        activeLiveSetCellIndexPlusOne_.store(0, std::memory_order_release);
        retainedLiveSetEventValues_.clear();
        retainedRootEventValueTargetIndices_.clear();
        retainedRootEventValueBaseline_.clear();
        retainedLiveSetEventValueCount_ = 0;
        rootPendingEventValueDirty_.reset();
        rootPendingEventValueDesired_.reset();
        rootCallbackEventValueDirty_.clear();
        rootCallbackEventValueDesired_.clear();
        preparedLiveSetRuntime_.reset();
        preparedLiveSet_.reset();
        preparedLiveTools_.reset();
        liveSetOldScratch_.clear();
        liveSetNewScratch_.clear();
        for (auto& track : liveSetRoutedInputScratch_)
            track.clear();
        liveSetRoutedInputPointers_.clear();
        liveSetOldPointers_.clear();
        liveSetNewPointers_.clear();
        return true;
    }
    auto runtime = std::make_unique<PreparedLiveSetRuntime>(*prepared, *this);
    if (!runtime->ready())
        return false;
    std::size_t retainedEventValueCapacity =
        rootEventValueBindingCount_;
    for (const auto& cell : prepared->cells())
        if (const auto* sound = std::get_if<PreparedLiveSetSoundProgram>(
                &cell.program);
            sound != nullptr && sound->graph != nullptr)
            retainedEventValueCapacity = std::max(
                retainedEventValueCapacity,
                sound->graph->renderSettings().eventValues.size());
    std::vector<RealtimeEventValueUpdate> retainedEventValues(
        retainedEventValueCapacity);
    std::vector<std::uint32_t> retainedRootIndices(
        retainedEventValueCapacity, 0);
    std::vector<double> retainedRootBaseline(
        retainedEventValueCapacity, 0.0);
    auto pendingRootDirty = std::make_unique<std::atomic<bool>[]>(
        rootEventValueBindingCount_);
    auto pendingRootDesired = std::make_unique<std::atomic<double>[]>(
        rootEventValueBindingCount_);
    std::vector<std::uint8_t> callbackRootDirty(
        rootEventValueBindingCount_, 0);
    std::vector<double> callbackRootDesired(
        rootEventValueBindingCount_, 0.0);
    for (std::size_t index = 0; index < rootEventValueBindingCount_; ++index)
    {
        pendingRootDirty[index].store(false, std::memory_order_relaxed);
        pendingRootDesired[index].store(0.0, std::memory_order_relaxed);
    }
    const auto rootValueCount = session_.captureCurrentGlobalEventValues(
        retainedEventValues);
    if (!rootValueCount.has_value())
        return false;
    for (std::size_t index = 0; index < *rootValueCount; ++index)
    {
        const auto& value = retainedEventValues[index];
        const auto target = session_.eventValueTargetIndex(
            value.name, value.domain);
        if (!target.has_value())
            return false;
        retainedRootIndices[index] = *target;
        retainedRootBaseline[index] = value.value;
    }
    const auto scratchChannelCount =
        std::max(scratchChannels_.size(), runtime->maximumOutputChannels());
    std::vector<std::vector<float>> oldScratch(scratchChannelCount,
                                               std::vector<float>(maxBlockFrames(), 0.0F));
    auto newScratch = oldScratch;
    std::vector<float*> oldPointers(scratchChannelCount, nullptr);
    std::vector<float*> newPointers(scratchChannelCount, nullptr);
    for (std::size_t channel = 0; channel < scratchChannelCount; ++channel)
    {
        oldPointers[channel] = oldScratch[channel].data();
        newPointers[channel] = newScratch[channel].data();
    }
    decltype(liveSetRoutedInputScratch_) routedInputScratch;
    std::vector<const float*> routedInputPointers;
    if (std::ranges::any_of(
            prepared->cells(), [&](const PreparedLiveSetCell& cell) {
                const auto* routing = prepared->trackRouting(cell.liveSetId);
                return routing != nullptr && routing->hasRoutes;
            }))
    {
        routedInputPointers.resize(scratchChannelCount, nullptr);
        for (auto& track : routedInputScratch)
        {
            track.resize(scratchChannelCount);
            for (auto& channel : track)
                channel.resize(maxBlockFrames(), 0.0F);
        }
    }
    runtime->bindAuthority(stageId_, snapshotGeneration_);
    activeLiveSetEngine_.store(nullptr, std::memory_order_release);
    liveSetControlEngine_.store(nullptr, std::memory_order_release);
    liveSetTransitionFrom_ = nullptr;
    liveSetTransitionTo_ = nullptr;
    liveSetTransitionCellIndexPlusOne_ = 0;
    liveSetTransitionFrames_ = 0;
    liveSetTransitionCompletedFrames_ = 0;
    activeLiveSetCellIndexPlusOne_.store(0, std::memory_order_release);
    retainedLiveSetEventValues_ = std::move(retainedEventValues);
    retainedRootEventValueTargetIndices_ = std::move(retainedRootIndices);
    retainedRootEventValueBaseline_ = std::move(retainedRootBaseline);
    retainedLiveSetEventValueCount_ = *rootValueCount;
    rootPendingEventValueDirty_ = std::move(pendingRootDirty);
    rootPendingEventValueDesired_ = std::move(pendingRootDesired);
    rootCallbackEventValueDirty_ = std::move(callbackRootDirty);
    rootCallbackEventValueDesired_ = std::move(callbackRootDesired);
    preparedLiveSetRuntime_ = std::move(runtime);
    preparedLiveSet_ = std::move(prepared);
    liveSetOldScratch_ = std::move(oldScratch);
    liveSetNewScratch_ = std::move(newScratch);
    liveSetRoutedInputScratch_ = std::move(routedInputScratch);
    liveSetRoutedInputPointers_ = std::move(routedInputPointers);
    liveSetOldPointers_ = std::move(oldPointers);
    liveSetNewPointers_ = std::move(newPointers);
    return true;
}

const PreparedLiveSet* RealtimeAudioEngine::preparedLiveSet() const noexcept
{
    return preparedLiveSet_.get();
}

bool RealtimeAudioEngine::setPreparedLiveTools(
    std::shared_ptr<const PreparedLiveTools> prepared) noexcept
{
    if (prepared != nullptr
        && (preparedLiveSet_ == nullptr
            || prepared->binding() != preparedLiveSet_->binding()))
    {
        return false;
    }
    preparedLiveTools_ = std::move(prepared);
    return true;
}

const PreparedLiveTools* RealtimeAudioEngine::preparedLiveTools() const noexcept
{
    return preparedLiveTools_.get();
}

bool RealtimeAudioEngine::activatePreparedLiveSetProgram(std::size_t cellIndex,
                                                         std::uint64_t absoluteFrame,
                                                         std::uint64_t transitionFrames) noexcept
{
    if (preparedLiveSetRuntime_ == nullptr || liveSetTransitionTo_ != nullptr)
        return false;
    auto* candidate = preparedLiveSetRuntime_->engineAt(cellIndex);
    if (candidate == nullptr)
        return false;
    const auto& cell = preparedLiveSet_->cells()[cellIndex];
    const auto soundProgram = std::holds_alternative<
        PreparedLiveSetSoundProgram>(cell.program);
    const auto activeCellIndexPlusOne =
        activeLiveSetCellIndexPlusOne_.load(std::memory_order_relaxed);
    if (soundProgram && activeCellIndexPlusOne != 0
        && activeCellIndexPlusOne <= preparedLiveSet_->cells().size()
        && preparedLiveSet_->cells()[activeCellIndexPlusOne - 1U]
               .liveSetId != cell.liveSetId)
    {
        const auto* selected = activeLiveSetEngine_.load(
            std::memory_order_acquire);
        const RealtimeAudioEngine* source = nullptr;
        for (const auto& slot : activeLiveSetSoundByColumn_)
            if (auto* sound = slot.load(std::memory_order_acquire))
                if (source == nullptr || sound == selected)
                    source = sound;
        if (source != nullptr && !retainLiveSetEventValuesFrom(*source))
            return false;
        candidate->resetTransport();
        if (preparedLiveSetRuntime_->transfersPerformanceState(cellIndex)
            && !candidate->transferPerformanceStateFrom(*this))
            return false;
        candidate->synchronizeTransportFrame(absoluteFrame);
        if (!scheduleRetainedLiveSetEventValuesOn(*candidate))
        {
            candidate->resetTransport();
            return false;
        }
        deactivatePreparedLiveSetProgram(absoluteFrame);
        candidate->setPlaying(playing_.load(std::memory_order_acquire));
        if (cell.column < activeLiveSetSoundByColumn_.size())
        {
            activeLiveSetSoundByColumn_[cell.column].store(
                candidate, std::memory_order_release);
            activeLiveSetSoundCellByColumn_[cell.column].store(
                static_cast<std::uint32_t>(cellIndex) + 1U,
                std::memory_order_release);
        }
        activeLiveSetEngine_.store(candidate, std::memory_order_release);
        liveSetControlEngine_.store(candidate, std::memory_order_release);
        activeLiveSetCellIndexPlusOne_.store(
            static_cast<std::uint32_t>(cellIndex) + 1U,
            std::memory_order_release);
        return true;
    }
    const auto activeSoundCount = std::ranges::count_if(
        activeLiveSetSoundByColumn_,
        [](const auto& slot) {
            return slot.load(std::memory_order_acquire) != nullptr;
        });
    const auto* routing = preparedLiveSet_->trackRouting(cell.liveSetId);
    if (!soundProgram && activeSoundCount > 1)
        return false;
    if (soundProgram && routing != nullptr && routing->hasRoutes
        && activeSoundCount == 0
        && cell.column < activeLiveSetSoundByColumn_.size())
    {
        auto* previous = activeLiveSetEngine_.load(
            std::memory_order_acquire);
        candidate->resetTransport();
        candidate->synchronizeTransportFrame(absoluteFrame);
        if (!scheduleRetainedLiveSetEventValuesOn(*candidate))
        {
            candidate->resetTransport();
            return false;
        }
        candidate->setPlaying(playing_.load(std::memory_order_acquire));
        if (previous != nullptr && previous != candidate)
            previous->setPlaying(false);
        activeLiveSetSoundByColumn_[cell.column].store(
            candidate, std::memory_order_release);
        activeLiveSetSoundCellByColumn_[cell.column].store(
            static_cast<std::uint32_t>(cellIndex) + 1U,
            std::memory_order_release);
        activeLiveSetEngine_.store(candidate, std::memory_order_release);
        liveSetControlEngine_.store(candidate, std::memory_order_release);
        activeLiveSetCellIndexPlusOne_.store(
            static_cast<std::uint32_t>(cellIndex) + 1U,
            std::memory_order_release);
        return true;
    }
    if (soundProgram
        && cell.column < activeLiveSetSoundByColumn_.size())
    {
        if (activeSoundCount != 0)
        {
            auto* prior = activeLiveSetSoundByColumn_[cell.column].load(
                std::memory_order_acquire);
            if (prior != candidate)
            {
                candidate->resetTransport();
                candidate->synchronizeTransportFrame(absoluteFrame);
                const RealtimeAudioEngine* source = prior;
                if (source == nullptr)
                    for (const auto& slot : activeLiveSetSoundByColumn_)
                        if (auto* active = slot.load(
                                std::memory_order_acquire);
                            active != nullptr && active != candidate)
                        {
                            source = active;
                            break;
                        }
                if (source != nullptr
                    && (source->stageId_ != candidate->stageId_
                        || source->snapshotGeneration_
                            != candidate->snapshotGeneration_
                        || !candidate->session_
                                .scheduleMatchingGlobalEventValuesFrom(
                                    source->session_)))
                {
                    candidate->resetTransport();
                    return false;
                }
                candidate->setPlaying(
                    playing_.load(std::memory_order_acquire));
            }
            else if (!preparedLiveSetRuntime_->transfersPerformanceState(
                         cellIndex))
            {
                if (!retainLiveSetEventValuesFrom(*candidate))
                    return false;
                if (!candidate->session_
                         .canScheduleGlobalEventValuesAfterReset(
                             {retainedLiveSetEventValues_.data(),
                              retainedLiveSetEventValueCount_}))
                    return false;
                candidate->resetTransport();
                candidate->synchronizeTransportFrame(absoluteFrame);
                if (!scheduleRetainedLiveSetEventValuesOn(*candidate))
                {
                    candidate->resetTransport();
                    return false;
                }
            }
            if (prior != nullptr && prior != candidate)
                prior->setPlaying(false);
            activeLiveSetSoundByColumn_[cell.column].store(
                candidate, std::memory_order_release);
            activeLiveSetSoundCellByColumn_[cell.column].store(
                static_cast<std::uint32_t>(cellIndex) + 1U,
                std::memory_order_release);
            activeLiveSetEngine_.store(
                candidate, std::memory_order_release);
            liveSetControlEngine_.store(
                candidate, std::memory_order_release);
            activeLiveSetCellIndexPlusOne_.store(
                static_cast<std::uint32_t>(cellIndex) + 1U,
                std::memory_order_release);
            return true;
        }
    }
    auto* previous = activeLiveSetEngine_.load(std::memory_order_relaxed);
    const auto previousCellIndexPlusOne =
        activeLiveSetCellIndexPlusOne_.load(std::memory_order_relaxed);
    const auto transfersPerformanceState =
        preparedLiveSetRuntime_->transfersPerformanceState(cellIndex);
    if (previous == candidate)
    {
        if (!transfersPerformanceState)
            candidate->resetTransport();
        candidate->synchronizeTransportFrame(absoluteFrame);
        return true;
    }
    candidate->resetTransport();
    const RealtimeAudioEngine* transferSource = this;
    if (transfersPerformanceState && previous != nullptr
        && previousCellIndexPlusOne != 0
        && preparedLiveSetRuntime_->transfersPerformanceState(
            previousCellIndexPlusOne - 1U))
    {
        transferSource = previous;
    }
    if (transfersPerformanceState
        && !candidate->transferPerformanceStateFrom(*transferSource))
    {
        return false;
    }
    candidate->synchronizeTransportFrame(absoluteFrame);
    if (soundProgram && !scheduleRetainedLiveSetEventValuesOn(*candidate))
    {
        candidate->resetTransport();
        return false;
    }
    candidate->setPlaying(playing_.load(std::memory_order_acquire));
    liveSetControlEngine_.store(candidate, std::memory_order_release);
    if (!liveSetOldPointers_.empty() && liveSetOldPointers_.size() == liveSetNewPointers_.size())
    {
        liveSetTransitionFrom_ = previous;
        liveSetTransitionTo_ = candidate;
        liveSetTransitionCellIndexPlusOne_ = static_cast<std::uint32_t>(cellIndex) + 1U;
        liveSetTransitionFrames_ = transitionFrames;
        liveSetTransitionCompletedFrames_ = 0;
        return true;
    }
    if (previous != nullptr)
        previous->setPlaying(false);
    activeLiveSetEngine_.store(candidate, std::memory_order_release);
    activeLiveSetCellIndexPlusOne_.store(static_cast<std::uint32_t>(cellIndex) + 1U,
                                         std::memory_order_release);
    if (soundProgram && cell.column < activeLiveSetSoundByColumn_.size())
    {
        activeLiveSetSoundByColumn_[cell.column].store(
            candidate, std::memory_order_release);
        activeLiveSetSoundCellByColumn_[cell.column].store(
            static_cast<std::uint32_t>(cellIndex) + 1U,
            std::memory_order_release);
    }
    return true;
}

std::uint32_t RealtimeAudioEngine::activePreparedLiveSetCellIndexPlusOne() const noexcept
{
    return activeLiveSetCellIndexPlusOne_.load(std::memory_order_acquire);
}

std::uint32_t RealtimeAudioEngine::activePreparedLiveSetSoundCellIndexPlusOne(
    std::uint32_t column) const noexcept
{
    return column < activeLiveSetSoundCellByColumn_.size()
        ? activeLiveSetSoundCellByColumn_[column].load(
            std::memory_order_acquire)
        : 0;
}

std::size_t RealtimeAudioEngine::activePreparedLiveSetSoundCount() const noexcept
{
    return std::ranges::count_if(
        activeLiveSetSoundByColumn_,
        [](const auto& slot) {
            return slot.load(std::memory_order_acquire) != nullptr;
        });
}

bool RealtimeAudioEngine::setPreparedLiveSetSoundColumnMuted(
    std::uint32_t column, bool muted) noexcept
{
    if (column >= activeLiveSetSoundByColumn_.size()
        || activeLiveSetSoundByColumn_[column].load(
               std::memory_order_acquire) == nullptr)
        return false;
    liveSetSoundMutedByColumn_[column].store(
        muted, std::memory_order_release);
    return true;
}

bool RealtimeAudioEngine::preparedLiveSetSoundColumnMuted(
    std::uint32_t column) const noexcept
{
    return column < liveSetSoundMutedByColumn_.size()
        && liveSetSoundMutedByColumn_[column].load(
            std::memory_order_acquire);
}

bool RealtimeAudioEngine::preparedLiveSetActivationPending() const noexcept
{
    return liveSetTransitionTo_ != nullptr;
}

void RealtimeAudioEngine::deactivatePreparedLiveSetProgram(std::uint64_t absoluteFrame) noexcept
{
    const RealtimeAudioEngine* lastSound = nullptr;
    const auto* selected = activeLiveSetEngine_.load(
        std::memory_order_acquire);
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire))
        {
            if (lastSound == nullptr || sound == selected)
                lastSound = sound;
        }
    if (lastSound != nullptr
        && !retainLiveSetEventValuesFrom(*lastSound))
        retainedLiveSetEventValueCount_ = 0;
    for (std::size_t column = 0;
         column < activeLiveSetSoundByColumn_.size();
         ++column)
    {
        if (auto* sound = activeLiveSetSoundByColumn_[column].exchange(
                nullptr, std::memory_order_acq_rel))
        {
            sound->setPlaying(false);
        }
        activeLiveSetSoundCellByColumn_[column].store(
            0, std::memory_order_release);
        liveSetSoundMutedByColumn_[column].store(
            false, std::memory_order_release);
    }
    if (liveSetTransitionTo_ != nullptr)
        liveSetTransitionTo_->setPlaying(false);
    liveSetTransitionFrom_ = nullptr;
    liveSetTransitionTo_ = nullptr;
    liveSetTransitionCellIndexPlusOne_ = 0;
    liveSetTransitionFrames_ = 0;
    liveSetTransitionCompletedFrames_ = 0;
    if (auto* previous = activeLiveSetEngine_.exchange(nullptr, std::memory_order_acq_rel))
    {
        previous->setPlaying(false);
    }
    liveSetControlEngine_.store(nullptr, std::memory_order_release);
    activeLiveSetCellIndexPlusOne_.store(0, std::memory_order_release);
    session_.synchronizeTransportFrame(absoluteFrame);
}

RealtimeStreamHealthSnapshot
RealtimeAudioEngine::streamHealth() const
{
    if (const auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return live->streamHealth();
    }
    auto result = session_.streamHealth();
    result.stageId = stageId_;
    return result;
}

PreparedNodeTimingSnapshot
RealtimeAudioEngine::preparedNodeTiming() const
{
    if (const auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return live->preparedNodeTiming();
    }
    auto result = session_.preparedNodeTiming();
    result.stageId = stageId_;
    return result;
}

PreparedVariableValueSnapshot
RealtimeAudioEngine::preparedVariableValues() const
{
    if (const auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return live->preparedVariableValues();
    }
    auto result = session_.preparedVariableValues();
    result.stageId = stageId_;
    result.publicationValid = result.publicationValid && stageId_ != 0;
    return result;
}

PreparedSoundVariableValueSnapshot RealtimeAudioEngine::preparedSoundVariableValues() const
{
    if (const auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return live->preparedSoundVariableValues();
    }
    auto result = session_.preparedSoundVariableValues();
    result.stageId = stageId_;
    result.publicationValid = result.publicationValid && stageId_ != 0;
    return result;
}

bool RealtimeAudioEngine::performanceStateCompatibleWith(
    const RealtimeAudioEngine& source) const noexcept
{
    return session_.performanceStateCompatibleWith(source.session_);
}

bool RealtimeAudioEngine::transferPerformanceStateFrom(
    const RealtimeAudioEngine& source) noexcept
{
    return session_.transferPerformanceStateFrom(source.session_);
}

std::optional<TransportLoopRegion> RealtimeAudioEngine::transportLoop() const noexcept
{
    return session_.transportLoop();
}

void RealtimeAudioEngine::setPreparedStageTransportFrame(
    std::uint64_t absoluteFrame) noexcept
{
    preparedStageFrame_ = absoluteFrame;
}

std::optional<std::uint64_t> RealtimeAudioEngine::preparedStageTransportFrame() const noexcept
{
    return preparedStageFrame_;
}

bool RealtimeAudioEngine::canAdoptAtTransportFrame(
    std::uint64_t absoluteFrame) const noexcept
{
    return !preparedStageFrame_ || absoluteFrame == *preparedStageFrame_;
}

std::optional<std::uint64_t> RealtimeAudioEngine::midiSongPositionFrame(
    std::uint16_t songPosition) const noexcept
{
    return session_.midiSongPositionFrame(songPosition);
}

std::optional<std::uint64_t> RealtimeAudioEngine::preparedScorePlaybackFrame() const noexcept
{
    return session_.preparedScorePlaybackFrame();
}

std::optional<talas::project::MusicalPosition>
RealtimeAudioEngine::musicalPositionAtFrame(
    std::uint64_t frame) const noexcept
{
    return session_.musicalPositionAtFrame(frame);
}

std::optional<talas::project::MusicalPosition>
RealtimeAudioEngine::musicalPositionAtTick(
    std::uint64_t tick) const noexcept
{
    return session_.musicalPositionAtTick(tick);
}

std::optional<RealtimeMusicalClockReading>
RealtimeAudioEngine::musicalClockAtFrame(
    std::uint64_t frame) const noexcept
{
    return session_.musicalClockAtFrame(frame);
}

std::optional<std::uint64_t> RealtimeAudioEngine::musicalTickFrame(
    std::uint64_t tick) const noexcept
{
    return session_.musicalTickFrame(tick);
}

void RealtimeAudioEngine::setStageId(std::uint64_t stageId) noexcept
{
    stageId_ = stageId;
    if (stageId != 0)
        static_cast<void>(session_.bindAnalysisStage(stageId));
    if (preparedLiveSetRuntime_ != nullptr)
        preparedLiveSetRuntime_->bindAuthority(stageId_, snapshotGeneration_);
}

std::uint64_t RealtimeAudioEngine::stageId() const noexcept
{
    return stageId_;
}

void RealtimeAudioEngine::setSnapshotGeneration(
    std::uint64_t generation) noexcept
{
    snapshotGeneration_ = generation;
    if (preparedLiveSetRuntime_ != nullptr)
        preparedLiveSetRuntime_->bindAuthority(stageId_, snapshotGeneration_);
}

std::uint64_t RealtimeAudioEngine::snapshotGeneration() const noexcept
{
    return snapshotGeneration_;
}

void RealtimeAudioEngine::setTransportSnapshotBeforeRender(
    RealtimeTransportSnapshot snapshot) noexcept
{
    session_.setTransportSnapshotBeforeRender(snapshot);
    auto* live = activeLiveSetEngine_.load(std::memory_order_acquire);
    if (live != nullptr)
    {
        live->setTransportSnapshotBeforeRender(snapshot);
    }
    if (liveSetTransitionTo_ != nullptr &&
        liveSetTransitionTo_ != live)
    {
        liveSetTransitionTo_->setTransportSnapshotBeforeRender(snapshot);
    }
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire);
            sound != nullptr && sound != live && sound != liveSetTransitionTo_)
            sound->setTransportSnapshotBeforeRender(snapshot);
}

void RealtimeAudioEngine::setTransportFrameBeforeFirstRender(std::uint64_t frame) noexcept
{
    if (!hasRendered_)
        session_.setTransportFrameBeforeFirstRender(frame);
    auto* live = activeLiveSetEngine_.load(std::memory_order_acquire);
    if (live != nullptr)
    {
        live->setTransportFrameBeforeFirstRender(frame);
    }
    if (liveSetTransitionTo_ != nullptr && liveSetTransitionTo_ != live)
        liveSetTransitionTo_->setTransportFrameBeforeFirstRender(frame);
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire);
            sound != nullptr && sound != live && sound != liveSetTransitionTo_)
            sound->setTransportFrameBeforeFirstRender(frame);
}

void RealtimeAudioEngine::synchronizeTransportFrame(std::uint64_t frame) noexcept
{
    session_.synchronizeTransportFrame(frame);
    auto* live = activeLiveSetEngine_.load(std::memory_order_acquire);
    if (live != nullptr)
    {
        live->synchronizeTransportFrame(frame);
    }
    if (liveSetTransitionTo_ != nullptr && liveSetTransitionTo_ != live)
        liveSetTransitionTo_->synchronizeTransportFrame(frame);
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire);
            sound != nullptr && sound != live && sound != liveSetTransitionTo_)
            sound->synchronizeTransportFrame(frame);
}

void RealtimeAudioEngine::resetTransport() noexcept
{
    session_.resetTransport();
    auto* live = activeLiveSetEngine_.load(std::memory_order_acquire);
    if (live != nullptr)
    {
        live->resetTransport();
    }
    if (liveSetTransitionTo_ != nullptr && liveSetTransitionTo_ != live)
        liveSetTransitionTo_->resetTransport();
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire);
            sound != nullptr && sound != live && sound != liveSetTransitionTo_)
            sound->resetTransport();
}

void RealtimeAudioEngine::setPlaying(bool playing) noexcept
{
    playing_.store(playing, std::memory_order_release);
    auto* live = activeLiveSetEngine_.load(std::memory_order_acquire);
    if (live != nullptr)
    {
        live->setPlaying(playing);
    }
    if (liveSetTransitionTo_ != nullptr && liveSetTransitionTo_ != live)
        liveSetTransitionTo_->setPlaying(playing);
    for (const auto& slot : activeLiveSetSoundByColumn_)
        if (auto* sound = slot.load(std::memory_order_acquire);
            sound != nullptr && sound != live && sound != liveSetTransitionTo_)
            sound->setPlaying(playing);
}

RealtimeAudioEngine* RealtimeAudioEngine::liveSetControlTarget() noexcept
{
    return liveSetControlEngine_.load(std::memory_order_acquire);
}

const RealtimeAudioEngine* RealtimeAudioEngine::liveSetControlTarget() const noexcept
{
    return liveSetControlEngine_.load(std::memory_order_acquire);
}

bool RealtimeAudioEngine::retainLiveSetEventValuesFrom(
    const RealtimeAudioEngine& source) noexcept
{
    if (stageId_ != source.stageId_
        || snapshotGeneration_ != source.snapshotGeneration_)
        return false;
    const auto count = source.session_.captureCurrentGlobalEventValues(
        retainedLiveSetEventValues_);
    if (!count.has_value())
        return false;
    retainedLiveSetEventValueCount_ = *count;
    resetRootEventValueBaselineForRetained();
    return true;
}

void RealtimeAudioEngine::resetRootEventValueBaselineForRetained() noexcept
{
    for (std::size_t index = 0;
         index < retainedLiveSetEventValueCount_; ++index)
    {
        const auto& retained = retainedLiveSetEventValues_[index];
        const auto target = session_.eventValueTargetIndex(
            retained.name, retained.domain);
        retainedRootEventValueTargetIndices_[index] =
            target.value_or(0);
        retainedRootEventValueBaseline_[index] =
            target.has_value()
            ? session_.currentGlobalEventValueAt(*target)
                  .value_or(0.0)
            : 0.0;
    }
}

void RealtimeAudioEngine::markPendingRootEventValue(
    std::uint32_t targetIndex, double value) noexcept
{
    if (rootPendingEventValueDirty_ == nullptr
        || rootPendingEventValueDesired_ == nullptr
        || targetIndex == 0 || targetIndex > rootEventValueBindingCount_
        || !std::isfinite(value))
        return;
    const auto index = targetIndex - 1U;
    rootPendingEventValueDesired_[index].store(
        value, std::memory_order_release);
    rootPendingEventValueDirty_[index].store(
        true, std::memory_order_release);
}

void RealtimeAudioEngine::beginRootEventValueCallback() noexcept
{
    for (std::size_t index = 0; index < rootEventValueBindingCount_; ++index)
    {
        rootCallbackEventValueDirty_[index] =
            rootPendingEventValueDirty_[index].exchange(
                false, std::memory_order_acq_rel) ? 1U : 0U;
        if (rootCallbackEventValueDirty_[index] != 0)
            rootCallbackEventValueDesired_[index] =
                rootPendingEventValueDesired_[index].load(
                    std::memory_order_acquire);
    }
}

void RealtimeAudioEngine::requeueRootEventValueCallback() noexcept
{
    for (std::size_t index = 0; index < rootEventValueBindingCount_; ++index)
        if (rootCallbackEventValueDirty_[index] != 0)
            rootPendingEventValueDirty_[index].store(
                true, std::memory_order_release);
}

void RealtimeAudioEngine::captureRenderedRootEventValueChanges() noexcept
{
    for (std::size_t index = 0;
         index < retainedLiveSetEventValueCount_; ++index)
    {
        const auto target = retainedRootEventValueTargetIndices_[index];
        if (target == 0)
            continue;
        const auto current = session_.currentGlobalEventValueAt(target);
        if (!current.has_value() || !std::isfinite(*current))
            continue;
        const auto rootIndex = target - 1U;
        const auto assigned =
            rootCallbackEventValueDirty_[rootIndex] != 0
            && *current == rootCallbackEventValueDesired_[rootIndex];
        if (assigned
            || *current != retainedRootEventValueBaseline_[index])
        {
            retainedLiveSetEventValues_[index].value = *current;
            retainedRootEventValueBaseline_[index] = *current;
        }
        if (rootCallbackEventValueDirty_[rootIndex] != 0 && !assigned)
            rootPendingEventValueDirty_[rootIndex].store(
                true, std::memory_order_release);
    }
}

bool RealtimeAudioEngine::scheduleRetainedLiveSetEventValuesOn(
    RealtimeAudioEngine& candidate) noexcept
{
    if (stageId_ != candidate.stageId_
        || snapshotGeneration_ != candidate.snapshotGeneration_)
        return false;
    for (std::size_t index = 0;
         index < retainedLiveSetEventValueCount_; ++index)
    {
        const auto& retained = retainedLiveSetEventValues_[index];
        const auto current = candidate.session_.currentGlobalEventValue(
            retained.name, retained.domain);
        if (!current.has_value() || *current == retained.value)
            continue;
        if (!candidate.session_.scheduleEventValue(
                retained.name, retained.value, retained.domain, 0))
            return false;
    }
    return true;
}

bool RealtimeAudioEngine::updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain)
{
    if (activePreparedLiveSetSoundCount() > 1)
    {
        bool matched = false;
        bool accepted = true;
        for (const auto& slot : activeLiveSetSoundByColumn_)
        {
            auto* sound = slot.load(std::memory_order_acquire);
            if (sound == nullptr
                || !sound->eventValueTarget(
                    name, domain, snapshotGeneration_).has_value())
                continue;
            matched = true;
            accepted = sound->updateEventValue(name, value, domain)
                && accepted;
        }
        return matched && accepted;
    }
    if (auto* live = liveSetControlTarget())
        return live->updateEventValue(name, value, domain);
    const auto accepted = session_.updateEventValue(name, value, domain);
    if (accepted && preparedLiveSetRuntime_ != nullptr)
        if (const auto target = session_.eventValueTargetIndex(
                name, domain))
            markPendingRootEventValue(*target, value);
    return accepted;
}

bool RealtimeAudioEngine::scheduleEventValue(
    std::string_view name,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset) noexcept
{
    if (activePreparedLiveSetSoundCount() > 1)
    {
        bool matched = false;
        bool accepted = true;
        for (const auto& slot : activeLiveSetSoundByColumn_)
        {
            auto* sound = slot.load(std::memory_order_acquire);
            if (sound == nullptr
                || !sound->eventValueTarget(
                    name, domain, snapshotGeneration_).has_value())
                continue;
            matched = true;
            accepted = sound->scheduleEventValue(
                name, value, domain, frameOffset) && accepted;
        }
        return matched && accepted;
    }
    if (auto* live = liveSetControlTarget())
        return live->scheduleEventValue(name, value, domain, frameOffset);
    const auto accepted = session_.scheduleEventValue(
        name, value, domain, frameOffset);
    if (accepted && preparedLiveSetRuntime_ != nullptr)
        if (const auto target = session_.eventValueTargetIndex(
                name, domain))
            markPendingRootEventValue(*target, value);
    return accepted;
}

bool RealtimeAudioEngine::scheduleEventValues(
    std::span<const RealtimeEventValueUpdate> values,
    std::size_t frameOffset) noexcept
{
    if (auto* live = liveSetControlTarget())
        return live->scheduleEventValues(values, frameOffset);
    const auto accepted = session_.scheduleEventValues(values, frameOffset);
    if (accepted && preparedLiveSetRuntime_ != nullptr)
        for (const auto& value : values)
            if (const auto target = session_.eventValueTargetIndex(
                    value.name, value.domain))
                markPendingRootEventValue(*target, value.value);
    return accepted;
}

SnapshotDirectTransactionResult
RealtimeAudioEngine::scheduleSnapshotDirectTransaction(
    const SnapshotDirectTransaction& transaction) noexcept
{
    if (stageId_ == 0 || snapshotGeneration_ == 0
        || transaction.stageId != stageId_
        || transaction.generation != snapshotGeneration_)
    {
        return SnapshotDirectTransactionResult::
            StaleStageOrGeneration;
    }
    const auto result = session_.scheduleSnapshotDirectTransaction(
        transaction.updates,
        transaction.variableUpdates,
        transaction.frameOffset,
        transaction.rampFrames);
    if (result == SnapshotDirectTransactionResult::Accepted
        && preparedLiveSetRuntime_ != nullptr)
        for (const auto& update : transaction.updates)
            markPendingRootEventValue(
                update.targetIndex, update.value);
    return result;
}

RealtimeMidiDispatchResult RealtimeAudioEngine::scheduleMidiNote(
    RealtimeMidiNoteEvent event,
    std::size_t frameOffset) noexcept
{
    if (auto* live = liveSetControlTarget())
        return live->scheduleMidiNote(event, frameOffset);
    return session_.scheduleMidiNote(event, frameOffset);
}

std::optional<TypedEventTarget> RealtimeAudioEngine::eventValueTarget(
    std::string_view name,
    talas::expr::ValueDomain domain,
    std::uint64_t generation) const noexcept
{
    if (const auto* live = liveSetControlTarget())
        return live->eventValueTarget(name, domain, generation);
    const auto index = session_.eventValueTargetIndex(name, domain);
    if (!index.has_value() || stageId_ == 0 || generation == 0)
        return std::nullopt;
    return TypedEventTarget{
        TypedEventTargetKind::EventValue,
        *index,
        stageId_,
        generation,
    };
}

std::optional<TypedEventTarget> RealtimeAudioEngine::eventValueTarget(
    RuntimeScopeId scopeId,
    std::string_view name,
    talas::expr::ValueDomain domain,
    std::uint64_t generation) const noexcept
{
    if (const auto* live = liveSetControlTarget())
    {
        return live->eventValueTarget(scopeId, name, domain, generation);
    }
    const auto index = session_.eventValueTargetIndex(
        scopeId,
        name,
        domain);
    if (!index.has_value() || stageId_ == 0 || generation == 0)
        return std::nullopt;
    return TypedEventTarget{
        TypedEventTargetKind::EventValue,
        *index,
        stageId_,
        generation,
    };
}

std::optional<TypedEventTarget> RealtimeAudioEngine::gateTarget(
    std::string_view nodeId,
    std::uint64_t generation) const noexcept
{
    if (const auto* live = liveSetControlTarget())
        return live->gateTarget(nodeId, generation);
    if (stageId_ == 0 || generation == 0)
        return std::nullopt;
    const auto index = session_.gateTargetIndex(nodeId);
    if (!index.has_value())
        return std::nullopt;
    return TypedEventTarget{
        TypedEventTargetKind::Gate,
        *index,
        stageId_,
        generation,
    };
}

std::optional<TypedEventTarget> RealtimeAudioEngine::variableTarget(
    const talas::project::PreparedProjectVariableIdentity& identity,
    talas::expr::ValueDomain domain,
    std::uint64_t generation) const noexcept
{
    if (const auto* live = liveSetControlTarget())
        return live->variableTarget(identity, domain, generation);
    const auto index = session_.variableTargetIndex(identity, domain);
    if (!index.has_value() || stageId_ == 0 || generation == 0)
        return std::nullopt;
    return TypedEventTarget{
        TypedEventTargetKind::Variable,
        *index,
        stageId_,
        generation,
    };
}

std::optional<SoundVariableTypedEventTarget> RealtimeAudioEngine::soundVariableTarget(
    const talas::project::PreparedProjectSoundVariableIdentity& identity,
    std::string_view selectedDefinitionId,
    std::uint64_t generation,
    std::uint64_t voiceGeneration
) const noexcept
{
    if (const auto* live = liveSetControlTarget())
    {
        return live->soundVariableTarget(
            identity, selectedDefinitionId, generation, voiceGeneration);
    }
    const auto resolved = voiceGeneration == 0
                              ? session_.soundVariableTargetIndex(identity, selectedDefinitionId)
                              : session_.voiceSoundVariableTargetIndex(
                                    identity, selectedDefinitionId, voiceGeneration
                                );
    if (!resolved.has_value() || stageId_ == 0 || generation == 0)
        return std::nullopt;
    return SoundVariableTypedEventTarget{
        {
            TypedEventTargetKind::SoundVariable,
            resolved->index,
            stageId_,
            generation,
            voiceGeneration,
        },
        resolved->selectedOrdinal,
    };
}

bool RealtimeAudioEngine::scheduleTypedEvents(
    std::span<const PreparedTypedEvent> events,
    std::span<RealtimeTypedEventDispatchResult> results) noexcept
{
    if (auto* live = liveSetControlTarget())
        return live->scheduleTypedEvents(events, results);
    if (results.size() != events.size() || stageId_ == 0)
        return false;
    for (const auto& event : events)
    {
        if (event.target.stageId != stageId_
            || event.target.generation == 0)
        {
            std::ranges::fill(
                results,
                RealtimeTypedEventDispatchResult::Rejected);
            return false;
        }
    }
    const auto accepted = session_.scheduleTypedEvents(events, results);
    if (accepted && preparedLiveSetRuntime_ != nullptr)
        for (std::size_t index = 0; index < events.size(); ++index)
            if (results[index] == RealtimeTypedEventDispatchResult::Handled
                && events[index].kind
                    == TypedEventKind::EventValueAssignment)
                markPendingRootEventValue(
                    events[index].target.index,
                    events[index].payload.eventValue.value);
    return accepted;
}

bool RealtimeAudioEngine::schedulePreparedLiveToolEvents(
    std::span<const PreparedTypedEvent> events,
    std::span<RealtimeTypedEventDispatchResult> results) noexcept
{
    if (results.size() != events.size() || stageId_ == 0)
        return false;
    for (const auto& event : events)
    {
        if (event.target.stageId != stageId_
            || event.target.generation != snapshotGeneration_)
        {
            std::ranges::fill(
                results,
                RealtimeTypedEventDispatchResult::Rejected);
            return false;
        }
    }
    const auto accepted = session_.scheduleTypedEvents(events, results);
    if (accepted && preparedLiveSetRuntime_ != nullptr)
        for (std::size_t index = 0; index < events.size(); ++index)
            if (results[index] == RealtimeTypedEventDispatchResult::Handled
                && events[index].kind
                    == TypedEventKind::EventValueAssignment)
                markPendingRootEventValue(
                    events[index].target.index,
                    events[index].payload.eventValue.value);
    return accepted;
}

bool RealtimeAudioEngine::resolvePreparedLiveToolEvent(
    std::string_view name,
    talas::expr::ExpressionValue& value) const noexcept
{
    return session_.resolveEventValue(name, value);
}

bool RealtimeAudioEngine::hasEventValueBinding(std::string_view name, talas::expr::ValueDomain domain) const noexcept
{
    if (const auto* live = liveSetControlTarget())
        return live->hasEventValueBinding(name, domain);
    return session_.hasEventValueBinding(name, domain);
}

bool RealtimeAudioEngine::eventValueRequiresReprepare(
    std::string_view name,
    talas::expr::ValueDomain domain) const noexcept
{
    if (const auto* live = liveSetControlTarget())
        return live->eventValueRequiresReprepare(name, domain);
    return session_.eventValueRequiresReprepare(name, domain);
}

bool RealtimeAudioEngine::canScheduleEventValue(
    std::string_view name,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset) noexcept
{
    if (auto* live = liveSetControlTarget())
    {
        return live->canScheduleEventValue(name, value, domain, frameOffset);
    }
    return session_.canScheduleEventValue(name, value, domain, frameOffset);
}

bool RealtimeAudioEngine::render(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept
{
    return render({}, outputChannels, channelCount, frameCount);
}

bool RealtimeAudioEngine::render(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount) noexcept
{
    return renderWithOutputMapping(
        input,
        outputChannels,
        channelCount,
        frameCount,
        true);
}

bool RealtimeAudioEngine::renderOrdered(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount) noexcept
{
    return renderWithOutputMapping(
        input,
        outputChannels,
        channelCount,
        frameCount,
        false);
}

RealtimeTerminalRenderResult RealtimeAudioEngine::renderTerminalEvents(
    std::size_t frameCount,
    RealtimeTerminalOutputBlock& output) noexcept
{
    hasRendered_ = true;
    if (liveSetTransitionTo_ != nullptr)
        return RealtimeTerminalRenderResult::WrongRenderKind;
    const auto activeCell = activeLiveSetCellIndexPlusOne_.load(
        std::memory_order_acquire);
    if (preparedLiveSet_ != nullptr && activeCell != 0
        && activeCell <= preparedLiveSet_->cells().size())
    {
        const auto* routing = preparedLiveSet_->trackRouting(
            preparedLiveSet_->cells()[activeCell - 1U].liveSetId);
        if (routing != nullptr && routing->hasRoutes)
            return RealtimeTerminalRenderResult::WrongRenderKind;
    }
    if (std::ranges::count_if(
            activeLiveSetSoundByColumn_,
            [](const auto& slot) {
                return slot.load(std::memory_order_acquire) != nullptr;
            }) > 1)
        return RealtimeTerminalRenderResult::WrongRenderKind;
    if (auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
        return live->renderTerminalEvents(frameCount, output);
    if (!playing_.load(std::memory_order_acquire)
        || !session_.ready()
        || frameCount == 0
        || frameCount > session_.maxBlockFrames())
    {
        return RealtimeTerminalRenderResult::InvalidArguments;
    }
    const auto result = session_.renderTerminalEvents(frameCount, output);
    if (result == RealtimeTerminalRenderResult::Rendered)
        output.stageId = stageId_;
    return result;
}

bool RealtimeAudioEngine::renderWithOutputMapping(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    bool duplicateMonoToStereo) noexcept
{
    hasRendered_ = true;
    diskRecorderTapFrames_ = 0;
    const auto commitTransitionSoundSlot = [&]() noexcept {
        if (preparedLiveSet_ == nullptr
            || liveSetTransitionCellIndexPlusOne_ == 0
            || liveSetTransitionCellIndexPlusOne_
                > preparedLiveSet_->cells().size())
        {
            return;
        }
        const auto& cell = preparedLiveSet_->cells()[
            liveSetTransitionCellIndexPlusOne_ - 1U];
        if (std::holds_alternative<PreparedLiveSetSoundProgram>(
                cell.program)
            && cell.column < activeLiveSetSoundByColumn_.size())
        {
            activeLiveSetSoundByColumn_[cell.column].store(
                liveSetTransitionTo_, std::memory_order_release);
            activeLiveSetSoundCellByColumn_[cell.column].store(
                liveSetTransitionCellIndexPlusOne_,
                std::memory_order_release);
        }
        else
        {
            for (std::size_t column = 0;
                 column < activeLiveSetSoundByColumn_.size();
                 ++column)
            {
                activeLiveSetSoundByColumn_[column].store(
                    nullptr, std::memory_order_release);
                activeLiveSetSoundCellByColumn_[column].store(
                    0, std::memory_order_release);
            }
        }
    };
    if (liveSetTransitionTo_ != nullptr)
    {
        // The crossfade scratch holds maxBlockFrames() frames per channel.
        if (outputChannels == nullptr || liveSetTransitionTo_ == nullptr ||
            frameCount == 0 || frameCount > maxBlockFrames() ||
            liveSetOldPointers_.empty() || liveSetOldPointers_.size() != liveSetNewPointers_.size())
        {
            clearOutputChannels(outputChannels, channelCount, frameCount);
            return false;
        }
        const auto scratchChannelCount = liveSetOldPointers_.size();
        const auto oldRendered =
            liveSetTransitionFrom_ != nullptr
                ? (duplicateMonoToStereo
                       ? liveSetTransitionFrom_->render(
                             input, liveSetOldPointers_.data(), scratchChannelCount, frameCount)
                       : liveSetTransitionFrom_->renderOrdered(
                             input, liveSetOldPointers_.data(), scratchChannelCount, frameCount))
                : renderOwnWithOutputMapping(input,
                                             liveSetOldPointers_.data(),
                                             scratchChannelCount,
                                             frameCount,
                                             duplicateMonoToStereo);
        const auto newRendered =
            duplicateMonoToStereo
                ? liveSetTransitionTo_->render(
                      input, liveSetNewPointers_.data(), scratchChannelCount, frameCount)
                : liveSetTransitionTo_->renderOrdered(
                      input, liveSetNewPointers_.data(), scratchChannelCount, frameCount);
        const auto copyScratch = [&](const auto& scratch)
        {
            for (std::size_t channel = 0; channel < channelCount; ++channel)
            {
                auto* output = outputChannels[channel];
                if (output == nullptr)
                    continue;
                if (channel >= scratchChannelCount)
                    std::fill_n(output, frameCount, 0.0F);
                else
                    std::copy_n(scratch[channel].data(), frameCount, output);
            }
        };
        if (!newRendered)
        {
            copyScratch(liveSetOldScratch_);
            liveSetTransitionTo_->setPlaying(false);
            liveSetControlEngine_.store(liveSetTransitionFrom_, std::memory_order_release);
            liveSetTransitionFrom_ = nullptr;
            liveSetTransitionTo_ = nullptr;
            liveSetTransitionCellIndexPlusOne_ = 0;
            liveSetTransitionFrames_ = 0;
            liveSetTransitionCompletedFrames_ = 0;
            return oldRendered;
        }
        if (liveSetTransitionFrames_ == 0 || !oldRendered)
        {
            copyScratch(liveSetNewScratch_);
            if (liveSetTransitionFrom_ != nullptr)
                liveSetTransitionFrom_->setPlaying(false);
            commitTransitionSoundSlot();
            activeLiveSetEngine_.store(liveSetTransitionTo_, std::memory_order_release);
            activeLiveSetCellIndexPlusOne_.store(liveSetTransitionCellIndexPlusOne_,
                                                 std::memory_order_release);
            liveSetTransitionFrom_ = nullptr;
            liveSetTransitionTo_ = nullptr;
            liveSetTransitionCellIndexPlusOne_ = 0;
            liveSetTransitionFrames_ = 0;
            liveSetTransitionCompletedFrames_ = 0;
            return true;
        }
        for (std::size_t channel = 0; channel < channelCount; ++channel)
        {
            auto* output = outputChannels[channel];
            if (output == nullptr)
                continue;
            if (channel >= scratchChannelCount)
            {
                std::fill_n(output, frameCount, 0.0F);
                continue;
            }
            for (std::size_t frame = 0; frame < frameCount; ++frame)
            {
                const auto progressed =
                    liveSetTransitionCompletedFrames_ > std::numeric_limits<std::uint64_t>::max() -
                                                            static_cast<std::uint64_t>(frame + 1U)
                        ? std::numeric_limits<std::uint64_t>::max()
                        : liveSetTransitionCompletedFrames_ +
                              static_cast<std::uint64_t>(frame + 1U);
                const auto alpha =
                    static_cast<float>(std::min(progressed, liveSetTransitionFrames_)) /
                    static_cast<float>(liveSetTransitionFrames_);
                output[frame] = liveSetOldScratch_[channel][frame] * (1.0F - alpha) +
                                liveSetNewScratch_[channel][frame] * alpha;
            }
        }
        const auto remaining =
            liveSetTransitionFrames_ -
            std::min(liveSetTransitionCompletedFrames_, liveSetTransitionFrames_);
        liveSetTransitionCompletedFrames_ += std::min<std::uint64_t>(remaining, frameCount);
        if (liveSetTransitionCompletedFrames_ >= liveSetTransitionFrames_)
        {
            if (liveSetTransitionFrom_ != nullptr)
                liveSetTransitionFrom_->setPlaying(false);
            commitTransitionSoundSlot();
            activeLiveSetEngine_.store(liveSetTransitionTo_, std::memory_order_release);
            activeLiveSetCellIndexPlusOne_.store(liveSetTransitionCellIndexPlusOne_,
                                                 std::memory_order_release);
            liveSetTransitionFrom_ = nullptr;
            liveSetTransitionTo_ = nullptr;
            liveSetTransitionCellIndexPlusOne_ = 0;
            liveSetTransitionFrames_ = 0;
            liveSetTransitionCompletedFrames_ = 0;
        }
        return oldRendered && newRendered;
    }
    const auto activeSoundCount = std::ranges::count_if(
        activeLiveSetSoundByColumn_,
        [](const auto& slot) {
            return slot.load(std::memory_order_acquire) != nullptr;
        });
    const auto activeCell = activeLiveSetCellIndexPlusOne_.load(
        std::memory_order_acquire);
    const auto* routing = preparedLiveSet_ != nullptr
            && activeCell != 0
            && activeCell <= preparedLiveSet_->cells().size()
        ? preparedLiveSet_->trackRouting(
              preparedLiveSet_->cells()[activeCell - 1U].liveSetId)
        : nullptr;
    if (activeSoundCount != 0 && routing != nullptr
        && routing->hasRoutes)
    {
        const auto scratchChannels = liveSetNewScratch_.size();
        if (outputChannels == nullptr || scratchChannels == 0
            || liveSetRoutedInputPointers_.size() != scratchChannels
            || frameCount == 0 || frameCount > maxBlockFrames())
        {
            clearOutputChannels(outputChannels, channelCount, frameCount);
            return false;
        }
        for (std::size_t column = 0;
             column < routing->hasIncoming.size(); ++column)
        {
            if (!routing->hasIncoming[column])
                continue;
            for (auto& channel : liveSetRoutedInputScratch_[column])
                std::fill_n(channel.data(), frameCount, 0.0F);
        }
        clearOutputChannels(outputChannels, channelCount, frameCount);
        bool renderedAny = false;
        for (const auto column : routing->renderOrder)
        {
            auto* sound = activeLiveSetSoundByColumn_[column].load(
                std::memory_order_acquire);
            if (sound == nullptr)
                continue;
            RealtimeAudioInputBlock soundInput = input;
            if (routing->hasIncoming[column])
            {
                for (std::size_t channel = 0;
                     channel < scratchChannels; ++channel)
                    liveSetRoutedInputPointers_[channel] =
                        liveSetRoutedInputScratch_[column][channel].data();
                soundInput = {
                    liveSetRoutedInputPointers_.data(),
                    scratchChannels, nullptr};
            }
            const auto rendered = duplicateMonoToStereo
                ? sound->render(
                      soundInput, liveSetNewPointers_.data(),
                      scratchChannels, frameCount)
                : sound->renderOrdered(
                      soundInput, liveSetNewPointers_.data(),
                      scratchChannels, frameCount);
            if (!rendered)
            {
                clearOutputChannels(outputChannels, channelCount, frameCount);
                return false;
            }
            renderedAny = true;
            if (liveSetSoundMutedByColumn_[column].load(
                    std::memory_order_acquire))
                continue;
            const auto destinations = routing->destinations[column];
            if (destinations != 0)
            {
                for (std::size_t destination = 0;
                     destination < liveSetRoutedInputScratch_.size();
                     ++destination)
                {
                    if ((destinations & (1U << destination)) == 0)
                        continue;
                    auto& routedInput =
                        liveSetRoutedInputScratch_[destination];
                    for (std::size_t channel = 0;
                         channel < scratchChannels; ++channel)
                        for (std::size_t frame = 0;
                             frame < frameCount; ++frame)
                            routedInput[channel][frame] +=
                                liveSetNewScratch_[channel][frame];
                }
            }
            else
            {
                for (std::size_t channel = 0;
                     channel < std::min(channelCount, scratchChannels);
                     ++channel)
                {
                    auto* output = outputChannels[channel];
                    if (output == nullptr)
                        continue;
                    for (std::size_t frame = 0;
                         frame < frameCount; ++frame)
                        output[frame] += liveSetNewScratch_[channel][frame];
                }
            }
        }
        return renderedAny;
    }
    const auto anyMuted = std::ranges::any_of(
        liveSetSoundMutedByColumn_,
        [](const auto& muted) {
            return muted.load(std::memory_order_acquire);
        });
    if (activeSoundCount > 1 || (activeSoundCount == 1 && anyMuted))
    {
        if (outputChannels == nullptr || liveSetNewPointers_.empty()
            || frameCount == 0 || frameCount > maxBlockFrames())
        {
            clearOutputChannels(outputChannels, channelCount, frameCount);
            return false;
        }
        bool first = true;
        for (std::size_t column = 0;
             column < activeLiveSetSoundByColumn_.size(); ++column)
        {
            auto* sound = activeLiveSetSoundByColumn_[column].load(
                std::memory_order_acquire);
            if (sound == nullptr)
                continue;
            const auto rendered = duplicateMonoToStereo
                ? sound->render(
                      input, liveSetNewPointers_.data(),
                      liveSetNewPointers_.size(), frameCount)
                : sound->renderOrdered(
                      input, liveSetNewPointers_.data(),
                      liveSetNewPointers_.size(), frameCount);
            if (!rendered)
            {
                clearOutputChannels(
                    outputChannels, channelCount, frameCount);
                return false;
            }
            if (liveSetSoundMutedByColumn_[column].load(
                    std::memory_order_acquire))
                continue;
            for (std::size_t channel = 0; channel < channelCount;
                 ++channel)
            {
                auto* output = outputChannels[channel];
                if (output == nullptr)
                    continue;
                if (channel >= liveSetNewScratch_.size())
                {
                    if (first)
                        std::fill_n(output, frameCount, 0.0F);
                    continue;
                }
                const auto* source = liveSetNewScratch_[channel].data();
                if (first)
                    std::copy_n(source, frameCount, output);
                else
                    for (std::size_t frame = 0;
                         frame < frameCount; ++frame)
                        output[frame] += source[frame];
            }
            first = false;
        }
        if (first)
            clearOutputChannels(outputChannels, channelCount, frameCount);
        return true;
    }
    if (auto* live = activeLiveSetEngine_.load(std::memory_order_acquire))
    {
        return duplicateMonoToStereo
                   ? live->render(input, outputChannels, channelCount, frameCount)
                   : live->renderOrdered(input, outputChannels, channelCount, frameCount);
    }
    if (preparedLiveSetRuntime_ != nullptr)
        beginRootEventValueCallback();
    const auto rendered = renderOwnWithOutputMapping(
        input, outputChannels, channelCount, frameCount, duplicateMonoToStereo);
    if (rendered && preparedLiveSetRuntime_ != nullptr)
        captureRenderedRootEventValueChanges();
    else if (preparedLiveSetRuntime_ != nullptr)
        requeueRootEventValueCallback();
    return rendered;
}

bool RealtimeAudioEngine::renderOwnWithOutputMapping(RealtimeAudioInputBlock input,
                                                     float* const* outputChannels,
                                                     std::size_t channelCount,
                                                     std::size_t frameCount,
                                                     bool duplicateMonoToStereo) noexcept
{
    if (outputChannels == nullptr)
        return false;

    const auto preparedChannelCount = scratchChannels_.size();
    bool hasWritableMainOutput = false;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        if (outputChannels[channel] != nullptr
            && (channel < preparedChannelCount
                || (duplicateMonoToStereo
                    && preparedChannelCount == 1
                    && channel == 1)))
        {
            hasWritableMainOutput = true;
            break;
        }
    }
    if (!hasWritableMainOutput
        || !playing_.load(std::memory_order_acquire)
        || !session_.ready()
        || frameCount == 0
        || frameCount > session_.maxBlockFrames())
    {
        clearOutputChannels(outputChannels, channelCount, frameCount);
        return false;
    }

    if (!session_.render(
            input,
            scratchChannelPointers_.data(),
            scratchChannelPointers_.size(),
            frameCount))
    {
        clearOutputChannels(outputChannels, channelCount, frameCount);
        return false;
    }

    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        if (outputChannels[channel] == nullptr)
            continue;
        const std::vector<double>* source = nullptr;
        if (channel < scratchChannels_.size())
            source = &scratchChannels_[channel];
        else if (duplicateMonoToStereo
                 && scratchChannels_.size() == 1
                 && channel == 1)
            source = &scratchChannels_.front();
        if (source == nullptr)
        {
            std::fill_n(outputChannels[channel], frameCount, 0.0F);
            continue;
        }
        for (std::size_t frame = 0; frame < frameCount; ++frame)
            outputChannels[channel][frame] = static_cast<float>((*source)[frame]);
    }
    for (std::size_t tap = 0; tap < diskRecorderTapScratch_.size(); ++tap)
    {
        for (std::size_t channel = 0; channel < diskRecorderTapScratch_[tap].size(); ++channel)
        {
            const auto recorded = session_.diskRecorderTapChannel(tap, channel);
            auto& scratch = diskRecorderTapScratch_[tap][channel];
            for (std::size_t frame = 0; frame < frameCount && frame < recorded.size() && frame < scratch.size(); ++frame)
                scratch[frame] = static_cast<float>(recorded[frame]);
        }
    }
    diskRecorderTapFrames_ = frameCount;

    return true;
}
}
