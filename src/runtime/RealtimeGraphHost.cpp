#include "talas/runtime/RealtimeGraphHost.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <vector>

namespace talas::runtime
{
struct RealtimeGraphTransition
{
    RealtimeGraphTransition(
        std::unique_ptr<RealtimeAudioEngine> candidate,
        std::uint64_t frames)
        : next(std::move(candidate))
        , totalFrames(frames)
        , oldScratch(
              next->outputChannelCount(),
              std::vector<float>(next->maxBlockFrames(), 0.0F))
        , newScratch(
              next->outputChannelCount(),
              std::vector<float>(next->maxBlockFrames(), 0.0F))
        , oldPointers(oldScratch.size(), nullptr)
        , newPointers(newScratch.size(), nullptr)
    {
        for (std::size_t channel = 0; channel < oldScratch.size(); ++channel)
        {
            oldPointers[channel] = oldScratch[channel].data();
            newPointers[channel] = newScratch[channel].data();
        }
    }

    std::unique_ptr<RealtimeAudioEngine> next;
    std::uint64_t totalFrames = 0;
    std::uint64_t completedFrames = 0;
    std::vector<std::vector<float>> oldScratch;
    std::vector<std::vector<float>> newScratch;
    std::vector<float*> oldPointers;
    std::vector<float*> newPointers;
};

namespace
{
void clearOutputChannels(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept
{
    if (outputChannels == nullptr)
        return;
    for (std::size_t channel = 0; channel < channelCount; ++channel)
    {
        if (outputChannels[channel] != nullptr)
            std::fill_n(outputChannels[channel], frameCount, 0.0F);
    }
}

[[nodiscard]] std::uint64_t loopedTransportFrame(
    std::uint64_t absoluteFrame,
    const std::optional<TransportLoopRegion>& loop) noexcept
{
    if (!loop.has_value()
        || loop->endFrame <= loop->startFrame
        || absoluteFrame < loop->startFrame)
    {
        return absoluteFrame;
    }
    return loop->startFrame
        + ((absoluteFrame - loop->startFrame)
           % (loop->endFrame - loop->startFrame));
}

void incrementSaturated(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

void addSaturatedSingleWriter(
    std::atomic<std::uint64_t>& value,
    std::uint64_t increment) noexcept
{
    const auto current = value.load(std::memory_order_relaxed);
    const auto available =
        std::numeric_limits<std::uint64_t>::max() - current;
    value.store(
        current + std::min(available, increment),
        std::memory_order_relaxed);
}

struct RenderMetricsGuard
{
    RenderMetricsGuard(
        std::atomic<std::uint64_t>& nonFailures,
        std::atomic<std::uint64_t>& failures) noexcept
        : nonFailureCount(nonFailures), failureCount(failures) {}

    ~RenderMetricsGuard()
    {
        addSaturatedSingleWriter(
            succeeded ? nonFailureCount : failureCount,
            1);
    }

    std::atomic<std::uint64_t>& nonFailureCount;
    std::atomic<std::uint64_t>& failureCount;
    bool succeeded = false;
};

[[nodiscard]] std::uint64_t nextSnapshotInstanceIdentity() noexcept
{
    static std::atomic<std::uint64_t> next = 1;
    auto identity = next.fetch_add(1, std::memory_order_relaxed);
    if (identity == 0)
        identity = next.fetch_add(1, std::memory_order_relaxed);
    return identity;
}
}

RealtimeGraphHost::RealtimeGraphHost()
    : snapshotInstanceIdentity_(nextSnapshotInstanceIdentity())
{
}

RealtimeGraphHost::~RealtimeGraphHost()
{
    clearAfterAudioStopped();
}

std::unique_ptr<RealtimeGraphHost::PreparedTransportRelocation>
RealtimeGraphHost::prepareTransportRelocation(std::uint64_t frame, std::string& error) const
{
    error.clear();
    std::array<RealtimeAudioEngine*, 6> engines{};
    std::size_t count = 0;
    const auto append = [&](RealtimeAudioEngine* engine) {
        if (engine && std::find(engines.begin(), engines.begin() + count, engine) == engines.begin() + count)
            engines[count++] = engine;
    };
    // A bounded second scan covers the pending-to-active split-atomic transfer.
    // The single control producer cannot collect these owners during preparation.
    for (int scan = 0; scan < 2; ++scan)
    {
        append(active_.load(std::memory_order_acquire));
        append(pending_.load(std::memory_order_acquire));
        append(transitioning_.load(std::memory_order_acquire));
    }
    if (!count) { error = "No prepared engine for transport relocation"; return {}; }
    auto candidate = std::make_unique<PreparedTransportRelocation>();
    candidate->frame = frame;
    candidate->engines.reserve(count);
    std::uint64_t remaining = 1000000;
    for (std::size_t index = 0; index < count; ++index)
    {
        auto prepared = engines[index]->prepareTransportRelocation(frame, remaining, error);
        if (!prepared) return {};
        candidate->engines.push_back(std::move(prepared));
    }
    return candidate;
}

bool RealtimeGraphHost::installTransportRelocation(
    PreparedTransportRelocation& candidate, std::uint64_t expectedStageId) noexcept
{
    auto* active = active_.load(std::memory_order_acquire);
    auto* transition = transitioning_.load(std::memory_order_acquire);
    const auto* authority = transition ? transition : active;
    if (!authority || expectedStageId == 0 || authority->stageId() != expectedStageId) return false;
    const auto find = [&](const RealtimeAudioEngine* engine) {
        if (!engine) return static_cast<RealtimeAudioEngine::PreparedTransportRelocation*>(nullptr);
        for (auto& point : candidate.engines)
            if (engine->canInstallTransportRelocation(*point)) return point.get();
        return static_cast<RealtimeAudioEngine::PreparedTransportRelocation*>(nullptr);
    };
    auto* activePoint = find(active);
    auto* transitionPoint = transition != active ? find(transition) : nullptr;
    if ((active && !activePoint) || (transition && transition != active && !transitionPoint)) return false;
    if (active && !active->installTransportRelocation(*activePoint)) return false;
    if (transitionPoint && !transition->installTransportRelocation(*transitionPoint)) return false;
    relocateTransportAtNextRender(candidate.frame, expectedStageId);
    return true;
}

RealtimeGraphStageResult RealtimeGraphHost::tryStage(std::unique_ptr<RealtimeAudioEngine> engine) noexcept
{
    if (engine == nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::NullEngine;
    }
    if (!engine->ready())
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::UnreadyEngine;
    }
    if (pending_.load(std::memory_order_acquire) != nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::PendingEngineAwaitingActivation;
    }
    if (activeTransition_.load(std::memory_order_acquire) != nullptr
        || pendingTransition_.load(std::memory_order_acquire) != nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::PendingEngineAwaitingActivation;
    }
    if (retired_.load(std::memory_order_acquire) != nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::RetiredEngineAwaitingCollection;
    }

    auto* candidate = engine.release();
    auto* expectedHealthSource = static_cast<RealtimeAudioEngine*>(nullptr);
    healthSource_.compare_exchange_strong(
        expectedHealthSource,
        candidate,
        std::memory_order_release,
        std::memory_order_relaxed);
    // Initialize health before exposing the candidate: the audio consumer can
    // reject even the first guarded stage before tryStage returns.
    pending_.store(candidate, std::memory_order_release);
    return RealtimeGraphStageResult::Accepted;
}

RealtimeGraphStageResult
RealtimeGraphHost::tryStageSameTopologyTransition(
    std::unique_ptr<RealtimeAudioEngine> engine,
    std::uint64_t transitionFrames) noexcept
{
    if (engine == nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::NullEngine;
    }
    if (!engine->ready())
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::UnreadyEngine;
    }
    auto* active = active_.load(std::memory_order_acquire);
    if (transitionFrames == 0 || active == nullptr)
        return tryStage(std::move(engine));
    if (pending_.load(std::memory_order_acquire) != nullptr
        || pendingTransition_.load(std::memory_order_acquire) != nullptr
        || activeTransition_.load(std::memory_order_acquire) != nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::PendingEngineAwaitingActivation;
    }
    if (retired_.load(std::memory_order_acquire) != nullptr
        || retiredTransition_.load(std::memory_order_acquire) != nullptr)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::RetiredEngineAwaitingCollection;
    }
    if (active->sampleRate() != engine->sampleRate()
        || active->maxBlockFrames() != engine->maxBlockFrames()
        || active->outputChannelCount() != engine->outputChannelCount()
        || active->transportLoop() != engine->transportLoop())
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::UnreadyEngine;
    }

    RealtimeGraphTransition* transition = nullptr;
    try
    {
        transition = new RealtimeGraphTransition(
            std::move(engine),
            transitionFrames);
    }
    catch (const std::bad_alloc&)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::UnreadyEngine;
    }
    catch (const std::length_error&)
    {
        addSaturatedSingleWriter(stageRejectionCount_, 1);
        return RealtimeGraphStageResult::UnreadyEngine;
    }
    auto* candidate = transition->next.get();
    pendingTransition_.store(transition, std::memory_order_release);
    auto* expectedHealthSource = static_cast<RealtimeAudioEngine*>(nullptr);
    healthSource_.compare_exchange_strong(
        expectedHealthSource,
        candidate,
        std::memory_order_release,
        std::memory_order_relaxed);
    pending_.store(candidate, std::memory_order_release);
    return RealtimeGraphStageResult::Accepted;
}

std::uint64_t RealtimeGraphHost::discardedOutputChannelCount() const noexcept
{
    return discardedOutputChannelCount_.load(std::memory_order_acquire);
}

RealtimeGraphMetricsSnapshot
RealtimeGraphHost::realtimeMetricsRaw() const noexcept
{
    const auto nonFailures =
        renderNonFailureCount_.load(std::memory_order_relaxed);
    const auto failures =
        renderFailureCount_.load(std::memory_order_relaxed);
    return {
        realtimeMetricAddSaturated(nonFailures, failures),
        failures,
        realtimeMetricAddSaturated(
            stageRejectionCount_.load(std::memory_order_relaxed),
            preparedStageRejectionCount_.load(std::memory_order_relaxed)),
    };
}

std::size_t RealtimeGraphHost::activeOutputChannelCount() const noexcept
{
    return activeOutputChannelCount_.load(std::memory_order_acquire);
}

RealtimeStreamHealthSnapshot
RealtimeGraphHost::streamHealth() const
{
    auto* engine = healthSource_.load(std::memory_order_acquire);
    return engine != nullptr
        ? engine->streamHealth()
        : RealtimeStreamHealthSnapshot{};
}

PreparedNodeTimingSnapshot
RealtimeGraphHost::preparedNodeTiming() const
{
    auto* engine = healthSource_.load(std::memory_order_acquire);
    return engine != nullptr
        ? engine->preparedNodeTiming()
        : PreparedNodeTimingSnapshot{};
}

PreparedVariableValueSnapshot
RealtimeGraphHost::preparedVariableValues() const
{
    auto* engine = healthSource_.load(std::memory_order_acquire);
    return engine != nullptr
        ? engine->preparedVariableValues()
        : PreparedVariableValueSnapshot{};
}

PreparedSoundVariableValueSnapshot RealtimeGraphHost::preparedSoundVariableValues() const
{
    auto* engine = healthSource_.load(std::memory_order_acquire);
    return engine != nullptr ? engine->preparedSoundVariableValues()
                             : PreparedSoundVariableValueSnapshot{};
}

std::uint64_t RealtimeGraphHost::activeStageId() const noexcept
{
    return activeStageId_.load(std::memory_order_acquire);
}

std::uint64_t RealtimeGraphHost::lastRejectedPreparedStageId() const noexcept
{
    return lastRejectedPreparedStageId_.load(std::memory_order_acquire);
}

bool RealtimeGraphHost::sameTopologyTransitionActive() const noexcept
{
    return pendingTransition_.load(std::memory_order_acquire) != nullptr
        || activeTransition_.load(std::memory_order_acquire) != nullptr;
}

std::uint64_t
RealtimeGraphHost::sameTopologyTransitionCompletionToken() const noexcept
{
    return transitionCompletionToken_.load(std::memory_order_acquire);
}

void RealtimeGraphHost::setPlaying(bool playing) noexcept
{
    playing_.store(playing, std::memory_order_release);
}

bool RealtimeGraphHost::updateEventValue(std::string_view name, double value, talas::expr::ValueDomain domain)
{
    std::array<RealtimeAudioEngine*, 6> engines{};
    std::size_t engineCount = 0;
    const auto appendUnique = [&](RealtimeAudioEngine* engine) {
        if (engine == nullptr
            || std::find(
                   engines.begin(),
                   engines.begin()
                       + static_cast<std::ptrdiff_t>(engineCount),
                   engine)
                != engines.begin()
                    + static_cast<std::ptrdiff_t>(engineCount))
        {
            return;
        }
        engines[engineCount++] = engine;
    };

    const auto appendSnapshot = [&] {
        appendUnique(active_.load(std::memory_order_acquire));
        appendUnique(pending_.load(std::memory_order_acquire));
        appendUnique(transitioning_.load(std::memory_order_acquire));
    };
    // One adoption has one split-atomic transfer. If the first bounded scan
    // straddles it, the completed transfer is visible to the second scan.
    appendSnapshot();
    appendSnapshot();

    bool hasTarget = false;
    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (!engines[index]->hasEventValueBinding(name, domain))
            continue;
        hasTarget = true;
        if (engines[index]->eventValueRequiresReprepare(name, domain)
            || !engines[index]->canScheduleEventValue(
                name,
                value,
                domain,
                0))
        {
            return false;
        }
    }
    if (!hasTarget)
        return false;

    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (engines[index]->hasEventValueBinding(name, domain)
            && !engines[index]->updateEventValue(name, value, domain))
        {
            return false;
        }
    }
    return true;
}

bool RealtimeGraphHost::scheduleEventValue(
    std::string_view name,
    double value,
    talas::expr::ValueDomain domain,
    std::size_t frameOffset) noexcept
{
    std::array<RealtimeAudioEngine*, 6> engines{};
    std::size_t engineCount = 0;
    const auto appendUnique = [&](RealtimeAudioEngine* engine) {
        if (engine == nullptr || std::find(engines.begin(), engines.begin() + static_cast<std::ptrdiff_t>(engineCount), engine)
                != engines.begin() + static_cast<std::ptrdiff_t>(engineCount))
            return;
        engines[engineCount++] = engine;
    };

    const auto appendSnapshot = [&] {
        appendUnique(active_.load(std::memory_order_acquire));
        appendUnique(pending_.load(std::memory_order_acquire));
        appendUnique(transitioning_.load(std::memory_order_acquire));
    };
    appendSnapshot();
    appendSnapshot();

    bool hasTarget = false;
    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (!engines[index]->hasEventValueBinding(name, domain))
            continue;
        hasTarget = true;
        if (!engines[index]->canScheduleEventValue(name, value, domain, frameOffset))
            return false;
    }
    if (!hasTarget)
        return false;

    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (engines[index]->hasEventValueBinding(name, domain)
            && !engines[index]->scheduleEventValue(name, value, domain, frameOffset))
        {
            return false;
        }
    }
    return true;
}

SnapshotDirectTransactionResult
RealtimeGraphHost::scheduleSnapshotDirectTransaction(
    const SnapshotDirectTransaction& transaction) noexcept
{
    std::array<RealtimeAudioEngine*, 6> engines{};
    std::size_t engineCount = 0;
    const auto appendUnique = [&](RealtimeAudioEngine* engine) {
        if (engine == nullptr
            || std::find(
                   engines.begin(),
                   engines.begin()
                       + static_cast<std::ptrdiff_t>(engineCount),
                   engine)
                != engines.begin()
                    + static_cast<std::ptrdiff_t>(engineCount))
        {
            return;
        }
        engines[engineCount++] = engine;
    };
    const auto appendSnapshot = [&] {
        appendUnique(active_.load(std::memory_order_acquire));
        appendUnique(pending_.load(std::memory_order_acquire));
        appendUnique(transitioning_.load(std::memory_order_acquire));
    };
    appendSnapshot();
    appendSnapshot();

    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (engines[index]->stageId() == transaction.stageId
            && engines[index]->snapshotGeneration()
                == transaction.generation)
        {
            return engines[index]
                ->scheduleSnapshotDirectTransaction(transaction);
        }
    }
    return SnapshotDirectTransactionResult::
        StaleStageOrGeneration;
}

bool RealtimeGraphHost::scheduleTypedEvents(
    std::span<const PreparedTypedEvent> events,
    std::span<RealtimeTypedEventDispatchResult> results) noexcept
{
    std::ranges::fill(
        results,
        RealtimeTypedEventDispatchResult::Rejected);
    if (events.empty() || results.size() != events.size())
        return false;
    const auto stageId = events.front().target.stageId;
    const auto generation = events.front().target.generation;
    if (stageId == 0 || generation == 0
        || std::ranges::any_of(
            events,
            [&](const PreparedTypedEvent& event) {
                return event.target.stageId != stageId
                    || event.target.generation != generation;
            }))
    {
        return false;
    }
    std::array<RealtimeAudioEngine*, 6> engines{};
    std::size_t engineCount = 0;
    const auto appendUnique = [&](RealtimeAudioEngine* engine) {
        if (engine == nullptr
            || std::find(
                   engines.begin(),
                   engines.begin()
                       + static_cast<std::ptrdiff_t>(engineCount),
                   engine)
                != engines.begin()
                    + static_cast<std::ptrdiff_t>(engineCount))
        {
            return;
        }
        engines[engineCount++] = engine;
    };
    const auto appendSnapshot = [&] {
        appendUnique(active_.load(std::memory_order_acquire));
        appendUnique(pending_.load(std::memory_order_acquire));
        appendUnique(transitioning_.load(std::memory_order_acquire));
    };
    appendSnapshot();
    appendSnapshot();
    for (std::size_t index = 0; index < engineCount; ++index)
    {
        if (engines[index]->stageId() == stageId
            && engines[index]->snapshotGeneration() == generation)
        {
            return engines[index]->scheduleTypedEvents(events, results);
        }
    }
    return false;
}

std::uint64_t RealtimeGraphHost::transportFramePosition() const noexcept
{
    return transportFramePosition_.load(std::memory_order_acquire);
}

std::optional<std::uint64_t> RealtimeGraphHost::scorePlaybackFrame() const noexcept
{
    const auto frame = scorePlaybackFrame_.load(std::memory_order_acquire);
    return frame == noScorePlaybackFrame ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{frame};
}

double RealtimeGraphHost::transportSampleRate() const noexcept
{
    return transportSampleRate_.load(std::memory_order_acquire);
}

RealtimeTransportSnapshot RealtimeGraphHost::transportSnapshot() const noexcept
{
    return musicalTransportSnapshot().transport;
}

RealtimeMusicalTransportSnapshot
RealtimeGraphHost::musicalTransportSnapshot() const noexcept
{
    struct StableFallback
    {
        const RealtimeGraphHost* owner = nullptr;
        std::uint64_t ownerIdentity = 0;
        RealtimeMusicalTransportSnapshot snapshot;
    };
    thread_local StableFallback fallback;
    if (fallback.owner != this
        || fallback.ownerIdentity != snapshotInstanceIdentity_)
    {
        fallback.owner = this;
        fallback.ownerIdentity = snapshotInstanceIdentity_;
        fallback.snapshot = {};
    }
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt)
    {
        const auto generationBefore =
            transportSnapshotGeneration_.load(std::memory_order_acquire);
        if ((generationBefore & 1U) != 0U)
            continue;
        RealtimeMusicalTransportSnapshot snapshot;
        snapshot.transport = {
            .epoch = publishedTransportEpoch_.load(std::memory_order_acquire),
            .absoluteFrame = publishedTransportAbsoluteFrame_.load(std::memory_order_acquire),
            .playheadFrame = transportFramePosition_.load(std::memory_order_acquire),
            .sampleRate = transportSampleRate_.load(std::memory_order_acquire),
            .loopStartFrame = publishedTransportLoopStartFrame_.load(std::memory_order_acquire),
            .loopEndFrame = publishedTransportLoopEndFrame_.load(std::memory_order_acquire),
            .discontinuitySequence = publishedTransportDiscontinuitySequence_.load(std::memory_order_acquire),
            .playing = publishedTransportPlaying_.load(std::memory_order_acquire),
            .loopEnabled = publishedTransportLoopEnabled_.load(std::memory_order_acquire),
        };
        snapshot.stageId = publishedTransportStageId_.load(
            std::memory_order_acquire);
        if (publishedMusicalPositionAvailable_.load(
                std::memory_order_acquire))
        {
            snapshot.musicalPosition = talas::project::MusicalPosition{
                .bar = publishedMusicalBar_.load(
                    std::memory_order_acquire),
                .beat = publishedMusicalBeat_.load(
                    std::memory_order_acquire),
                .tick = publishedMusicalTick_.load(
                    std::memory_order_acquire),
            };
        }
        snapshot.externalAuthorityGeneration =
            publishedExternalAuthorityGeneration_.load(
                std::memory_order_acquire);
        snapshot.externalPulseSequence =
            publishedExternalPulseSequence_.load(
                std::memory_order_acquire);
        snapshot.externalIntervalNanoseconds =
            publishedExternalIntervalNanoseconds_.load(
                std::memory_order_acquire);
        snapshot.externalMicrosecondsPerQuarter =
            publishedExternalMicrosecondsPerQuarter_.load(
                std::memory_order_acquire);
        snapshot.externalPhaseErrorNanoseconds =
            publishedExternalPhaseErrorNanoseconds_.load(
                std::memory_order_acquire);
        snapshot.externalJitterNanoseconds =
            publishedExternalJitterNanoseconds_.load(
                std::memory_order_acquire);
        snapshot.externalAcquisitionIntervals =
            publishedExternalAcquisitionIntervals_.load(
                std::memory_order_acquire);
        snapshot.externalLastPulseTimeSeconds =
            publishedExternalLastPulseTimeSeconds_.load(
                std::memory_order_acquire);
        snapshot.externalLockState = publishedExternalLockState_.load(
            std::memory_order_acquire);
        snapshot.externalLastAnchor = publishedExternalAnchor_.load(
            std::memory_order_acquire);
        snapshot.externalAuthority = publishedExternalAuthority_.load(
            std::memory_order_acquire);
        snapshot.mtcAuthorityGeneration =
            publishedMtcAuthorityGeneration_.load(std::memory_order_acquire);
        snapshot.mtcStageId = publishedMtcStageId_.load(
            std::memory_order_acquire);
        snapshot.mtcMappedProjectFrame =
            publishedMtcMappedProjectFrame_.load(std::memory_order_acquire);
        snapshot.mtc.state = publishedMtcLockState_.load(
            std::memory_order_acquire);
        snapshot.mtc.identity.authorityGeneration =
            snapshot.mtcAuthorityGeneration;
        snapshot.mtc.identity.stageId = snapshot.mtcStageId;
        snapshot.mtc.identity.sourceGeneration =
            publishedMtcSourceGeneration_.load(std::memory_order_acquire);
        snapshot.mtc.identity.sourceSlot = publishedMtcSourceSlot_.load(
            std::memory_order_acquire);
        snapshot.mtc.timeCode = {
            .rate = publishedMtcRate_.load(std::memory_order_acquire),
            .dropFrame = publishedMtcDropFrame_.load(std::memory_order_acquire),
            .hour = publishedMtcHour_.load(std::memory_order_acquire),
            .minute = publishedMtcMinute_.load(std::memory_order_acquire),
            .second = publishedMtcSecond_.load(std::memory_order_acquire),
            .frame = publishedMtcFrame_.load(std::memory_order_acquire),
            .direction = publishedMtcDirection_.load(std::memory_order_acquire),
            .absoluteFrame = publishedMtcAbsoluteFrame_.load(std::memory_order_acquire),
        };
        snapshot.mtc.lastCompleteTimeSeconds =
            publishedMtcLastCompleteTimeSeconds_.load(std::memory_order_acquire);
        snapshot.mtc.completeSetSequence =
            publishedMtcCompleteSetSequence_.load(std::memory_order_acquire);
        snapshot.mtc.nextExpectedPiece =
            publishedMtcNextExpectedPiece_.load(std::memory_order_acquire);
        snapshot.mtc.discontinuityReason =
            publishedMtcDiscontinuityReason_.load(std::memory_order_acquire);
        snapshot.mtc.hasTimeCode = publishedMtcHasTimeCode_.load(
            std::memory_order_acquire);
        snapshot.mtc.runningEvidence =
            publishedMtcRunningEvidence_.load(std::memory_order_acquire);
        snapshot.mtcAuthority = publishedMtcAuthority_.load(
            std::memory_order_acquire);
        const auto generationAfter =
            transportSnapshotGeneration_.load(std::memory_order_acquire);
        if (generationBefore == generationAfter
            && (generationAfter & 1U) == 0U)
        {
            const auto lockStateValid =
                snapshot.externalLockState >= MidiClockLockState::Waiting
                && snapshot.externalLockState <= MidiClockLockState::Stale;
            const auto anchorValid =
                snapshot.externalLastAnchor >= MidiClockAnchorKind::None
                && snapshot.externalLastAnchor
                    <= MidiClockAnchorKind::SongPosition;
            const auto tempoValid =
                snapshot.externalMicrosecondsPerQuarter == 0
                || (snapshot.externalMicrosecondsPerQuarter >= 200'000
                    && snapshot.externalMicrosecondsPerQuarter
                        <= 3'000'000);
            const auto externalRecordValid =
                snapshot.externalAuthority
                    == (snapshot.externalAuthorityGeneration != 0)
                && std::isfinite(snapshot.externalLastPulseTimeSeconds)
                && snapshot.externalLastPulseTimeSeconds >= 0.0
                && snapshot.externalAcquisitionIntervals
                    <= midiClockAcquisitionIntervalCount
                && lockStateValid
                && anchorValid
                && tempoValid;
            const auto mtcLockValid =
                snapshot.mtc.state >= MtcTransportLockState::Waiting
                && snapshot.mtc.state <= MtcTransportLockState::Stale;
            const auto mtcReasonValid =
                snapshot.mtc.discontinuityReason
                    >= MtcDiscontinuityReason::None
                && snapshot.mtc.discontinuityReason
                    <= MtcDiscontinuityReason::ArithmeticOverflow;
            const auto mtcRecordValid =
                snapshot.mtcAuthority
                    == (snapshot.mtcAuthorityGeneration != 0)
                && (!snapshot.mtcAuthority
                    || (snapshot.mtc.identity.valid()
                        && snapshot.mtcStageId != 0
                        && snapshot.mtcStageId == snapshot.stageId
                        && snapshot.mtc.identity.stageId
                            == snapshot.mtcStageId
                        && snapshot.mtc.identity.authorityGeneration
                            == snapshot.mtcAuthorityGeneration))
                && std::isfinite(snapshot.mtc.lastCompleteTimeSeconds)
                && snapshot.mtc.lastCompleteTimeSeconds >= 0.0
                && snapshot.mtc.nextExpectedPiece
                    < mtcQuarterFramePieceCount
                && mtcLockValid && mtcReasonValid
                && (!snapshot.mtc.hasTimeCode
                    || isValidSmpteTimeCode(snapshot.mtc.timeCode))
                && (!snapshot.mtc.runningEvidence
                    || (snapshot.mtc.hasTimeCode
                        && snapshot.mtc.state
                            == MtcTransportLockState::Locked));
            const auto nonRegressed =
                fallback.snapshot.externalAuthorityGeneration
                        != snapshot.externalAuthorityGeneration
                    || fallback.snapshot.stageId != snapshot.stageId
                    || snapshot.externalPulseSequence
                        >= fallback.snapshot.externalPulseSequence;
            const auto mtcNonRegressed =
                fallback.snapshot.mtcAuthorityGeneration
                        != snapshot.mtcAuthorityGeneration
                    || fallback.snapshot.mtcStageId != snapshot.mtcStageId
                    || snapshot.mtc.completeSetSequence
                        >= fallback.snapshot.mtc.completeSetSequence;
            if (externalRecordValid && nonRegressed
                && mtcRecordValid && mtcNonRegressed)
            {
                snapshot.publicationValid = true;
                fallback.snapshot = snapshot;
                return snapshot;
            }
            // A stable record that fails structural or monotonic validation
            // is invalid evidence, not transient writer contention.
            return {};
        }
    }
    auto stable = fallback.snapshot;
    stable.usedStableFallback = true;
    return stable;
}

void RealtimeGraphHost::publishTransportSnapshot() noexcept
{
    transportSnapshotGeneration_.fetch_add(1, std::memory_order_acq_rel);
    const auto playheadFrame = loopedTransportFrame(
        absoluteTransportFramePosition_,
        activeTransportLoop_);
    auto* musicalSource =
        transitioning_.load(std::memory_order_acquire);
    if (musicalSource == nullptr)
        musicalSource = active_.load(std::memory_order_acquire);
    const auto externalMusicalPosition =
        audioThreadExternalMusicalTickAvailable_
            && musicalSource != nullptr
            && musicalSource->stageId()
                == audioThreadExternalMusicalStageId_
        ? musicalSource->musicalPositionAtTick(
              audioThreadExternalMusicalTick_)
        : std::optional<talas::project::MusicalPosition>{};
    const auto musicalPosition =
        audioThreadExternalMusicalTickAvailable_
        ? externalMusicalPosition
        : musicalSource != nullptr
            ? musicalSource->musicalPositionAtFrame(playheadFrame)
            : std::optional<talas::project::MusicalPosition>{};
    publishedTransportEpoch_.store(transportEpoch_, std::memory_order_release);
    publishedTransportAbsoluteFrame_.store(
        absoluteTransportFramePosition_,
        std::memory_order_release);
    transportFramePosition_.store(
        playheadFrame,
        std::memory_order_release);
    const auto scoreFrame = musicalSource != nullptr
        ? musicalSource->preparedScorePlaybackFrame()
        : std::optional<std::uint64_t>{};
    scorePlaybackFrame_.store(
        scoreFrame.value_or(noScorePlaybackFrame),
        std::memory_order_release);
    transportSampleRate_.store(
        activeTransportSampleRate_,
        std::memory_order_release);
    publishedTransportLoopStartFrame_.store(
        activeTransportLoop_.has_value() ? activeTransportLoop_->startFrame : 0,
        std::memory_order_release);
    publishedTransportLoopEndFrame_.store(
        activeTransportLoop_.has_value() ? activeTransportLoop_->endFrame : 0,
        std::memory_order_release);
    publishedTransportDiscontinuitySequence_.store(
        transportDiscontinuitySequence_,
        std::memory_order_release);
    publishedTransportPlaying_.store(
        playing_.load(std::memory_order_acquire),
        std::memory_order_release);
    publishedTransportLoopEnabled_.store(
        activeTransportLoop_.has_value(),
        std::memory_order_release);
    publishedTransportStageId_.store(
        musicalSource != nullptr ? musicalSource->stageId() : 0,
        std::memory_order_release);
    publishedMusicalBar_.store(
        musicalPosition.has_value() ? musicalPosition->bar : 0,
        std::memory_order_release);
    publishedMusicalBeat_.store(
        musicalPosition.has_value() ? musicalPosition->beat : 0,
        std::memory_order_release);
    publishedMusicalTick_.store(
        musicalPosition.has_value() ? musicalPosition->tick : 0,
        std::memory_order_release);
    publishedMusicalPositionAvailable_.store(
        musicalPosition.has_value(),
        std::memory_order_release);
    publishedExternalAuthorityGeneration_.store(
        audioThreadExternalAuthorityGeneration_,
        std::memory_order_release);
    publishedExternalPulseSequence_.store(
        audioThreadExternalFollower_.acceptedPulseSequence,
        std::memory_order_release);
    publishedExternalIntervalNanoseconds_.store(
        audioThreadExternalFollower_.intervalNanoseconds,
        std::memory_order_release);
    publishedExternalMicrosecondsPerQuarter_.store(
        audioThreadExternalFollower_.microsecondsPerQuarter,
        std::memory_order_release);
    publishedExternalPhaseErrorNanoseconds_.store(
        audioThreadExternalFollower_.phaseErrorNanoseconds,
        std::memory_order_release);
    publishedExternalJitterNanoseconds_.store(
        audioThreadExternalFollower_.jitterNanoseconds,
        std::memory_order_release);
    publishedExternalAcquisitionIntervals_.store(
        audioThreadExternalFollower_.acquisitionIntervals,
        std::memory_order_release);
    publishedExternalLastPulseTimeSeconds_.store(
        audioThreadExternalLastPulseTimeSeconds_,
        std::memory_order_release);
    publishedExternalLockState_.store(
        audioThreadExternalFollower_.state,
        std::memory_order_release);
    publishedExternalAnchor_.store(
        audioThreadExternalAnchor_,
        std::memory_order_release);
    publishedExternalAuthority_.store(
        audioThreadExternalAuthorityGeneration_ != 0,
        std::memory_order_release);
    publishedMtcAuthorityGeneration_.store(
        audioThreadExternalMtcAvailable_
            ? audioThreadExternalMtc_.identity.authorityGeneration
            : 0,
        std::memory_order_release);
    publishedMtcStageId_.store(
        audioThreadExternalMtcAvailable_
            ? audioThreadExternalMtc_.identity.stageId
            : 0,
        std::memory_order_release);
    publishedMtcSourceGeneration_.store(
        audioThreadExternalMtcAvailable_
            ? audioThreadExternalMtc_.identity.sourceGeneration
            : 0,
        std::memory_order_release);
    publishedMtcSourceSlot_.store(
        audioThreadExternalMtcAvailable_
            ? audioThreadExternalMtc_.identity.sourceSlot
            : 0,
        std::memory_order_release);
    publishedMtcMappedProjectFrame_.store(
        audioThreadExternalMtcMappedProjectFrame_,
        std::memory_order_release);
    publishedMtcAbsoluteFrame_.store(
        audioThreadExternalMtc_.timeCode.absoluteFrame,
        std::memory_order_release);
    publishedMtcCompleteSetSequence_.store(
        audioThreadExternalMtc_.completeSetSequence,
        std::memory_order_release);
    publishedMtcNextExpectedPiece_.store(
        audioThreadExternalMtc_.nextExpectedPiece,
        std::memory_order_release);
    publishedMtcLastCompleteTimeSeconds_.store(
        audioThreadExternalMtc_.lastCompleteTimeSeconds,
        std::memory_order_release);
    publishedMtcLockState_.store(
        audioThreadExternalMtc_.state,
        std::memory_order_release);
    publishedMtcDiscontinuityReason_.store(
        audioThreadExternalMtc_.discontinuityReason,
        std::memory_order_release);
    publishedMtcRate_.store(
        audioThreadExternalMtc_.timeCode.rate,
        std::memory_order_release);
    publishedMtcDropFrame_.store(
        audioThreadExternalMtc_.timeCode.dropFrame,
        std::memory_order_release);
    publishedMtcHour_.store(
        audioThreadExternalMtc_.timeCode.hour,
        std::memory_order_release);
    publishedMtcMinute_.store(
        audioThreadExternalMtc_.timeCode.minute,
        std::memory_order_release);
    publishedMtcSecond_.store(
        audioThreadExternalMtc_.timeCode.second,
        std::memory_order_release);
    publishedMtcFrame_.store(
        audioThreadExternalMtc_.timeCode.frame,
        std::memory_order_release);
    publishedMtcDirection_.store(
        audioThreadExternalMtc_.timeCode.direction,
        std::memory_order_release);
    publishedMtcHasTimeCode_.store(
        audioThreadExternalMtc_.hasTimeCode,
        std::memory_order_release);
    publishedMtcRunningEvidence_.store(
        audioThreadExternalMtc_.runningEvidence,
        std::memory_order_release);
    publishedMtcAuthority_.store(
        audioThreadExternalMtcAvailable_,
        std::memory_order_release);
    transportSnapshotGeneration_.fetch_add(1, std::memory_order_release);
}

void RealtimeGraphHost::requestTransportReset() noexcept
{
    transportResetGeneration_.fetch_add(1, std::memory_order_release);
}

void RealtimeGraphHost::relocateTransportAtNextRender(
    std::uint64_t absoluteFrame,
    std::uint64_t expectedStageId,
    std::uint16_t songPosition,
    std::uint64_t songPositionCount) noexcept
{
    audioThreadRelocationFrame_ = absoluteFrame;
    audioThreadRelocationExpectedStageId_ = expectedStageId;
    audioThreadRelocationSongPosition_ = songPosition;
    audioThreadRelocationSongPositionCount_ = songPositionCount;
    audioThreadRelocationPending_ = true;
    transportBoundaryPrepared_ = false;
}

void RealtimeGraphHost::setExternalMusicalTick(
    std::uint64_t tick,
    std::uint64_t expectedStageId,
    std::uint64_t authorityGeneration,
    MidiClockPhaseFollowerSnapshot follower,
    MidiClockAnchorKind anchor,
    double lastPulseTimeSeconds) noexcept
{
    audioThreadExternalMusicalTick_ = tick;
    audioThreadExternalMusicalStageId_ = expectedStageId;
    audioThreadExternalAuthorityGeneration_ = authorityGeneration;
    audioThreadExternalFollower_ = follower;
    audioThreadExternalAnchor_ = anchor;
    audioThreadExternalLastPulseTimeSeconds_ =
        std::isfinite(lastPulseTimeSeconds) && lastPulseTimeSeconds > 0.0
        ? lastPulseTimeSeconds
        : 0.0;
    audioThreadExternalMusicalTickAvailable_ = expectedStageId != 0;
}

void RealtimeGraphHost::clearExternalMusicalTick() noexcept
{
    audioThreadExternalMusicalTick_ = 0;
    audioThreadExternalMusicalStageId_ = 0;
    audioThreadExternalAuthorityGeneration_ = 0;
    audioThreadExternalFollower_ = {};
    audioThreadExternalAnchor_ = MidiClockAnchorKind::None;
    audioThreadExternalLastPulseTimeSeconds_ = 0.0;
    audioThreadExternalMusicalTickAvailable_ = false;
}

void RealtimeGraphHost::setExternalMtc(
    MtcTransportFollowerSnapshot follower,
    std::uint64_t mappedProjectFrame) noexcept
{
    audioThreadExternalMtc_ = follower;
    audioThreadExternalMtcMappedProjectFrame_ = mappedProjectFrame;
    audioThreadExternalMtcAvailable_ = follower.identity.valid();
}

void RealtimeGraphHost::clearExternalMtc() noexcept
{
    audioThreadExternalMtc_ = {};
    audioThreadExternalMtcMappedProjectFrame_ = 0;
    audioThreadExternalMtcAvailable_ = false;
}

bool RealtimeGraphHost::relocateMidiSongPositionAtNextRender(
    std::uint16_t songPosition) noexcept
{
    const auto frame = midiSongPositionFrame(songPosition);
    if (!frame.has_value())
        return false;
    relocateTransportAtNextRender(*frame);
    return true;
}

std::optional<std::uint64_t> RealtimeGraphHost::midiSongPositionFrame(
    std::uint16_t songPosition,
    std::uint64_t expectedStageId) const noexcept
{
    auto* source = pending_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr
            && (expectedStageId == 0
                || source->stageId() == expectedStageId)
        ? source->midiSongPositionFrame(songPosition)
        : std::optional<std::uint64_t>{};
}

std::uint64_t
RealtimeGraphHost::rejectedStageBoundRelocationCount() const noexcept
{
    return rejectedStageBoundRelocationCount_.load(
        std::memory_order_relaxed);
}

std::uint64_t
RealtimeGraphHost::lastStageBoundRelocationFrame() const noexcept
{
    return stageBoundRelocationSnapshot().lastFrame;
}

StageBoundRelocationSnapshot
RealtimeGraphHost::stageBoundRelocationSnapshot() const noexcept
{
    while (true)
    {
        const auto before = stageBoundRelocationGeneration_.load(
            std::memory_order_acquire);
        if ((before & 1U) != 0U)
            continue;
        const StageBoundRelocationSnapshot snapshot{
            .acceptedCount =
                acceptedStageBoundRelocationCount_.load(
                    std::memory_order_acquire),
            .lastSongPosition =
                lastStageBoundRelocationSongPosition_.load(
                    std::memory_order_acquire),
            .lastFrame = lastStageBoundRelocationFrame_.load(
                std::memory_order_acquire),
        };
        const auto after = stageBoundRelocationGeneration_.load(
            std::memory_order_acquire);
        if (before == after && (after & 1U) == 0U)
            return snapshot;
    }
}

std::unique_ptr<RealtimeAudioEngine> RealtimeGraphHost::collectRetired() noexcept
{
    auto retired = std::unique_ptr<RealtimeAudioEngine>(
        retired_.exchange(nullptr, std::memory_order_acq_rel));
    delete retiredTransition_.exchange(
        nullptr,
        std::memory_order_acq_rel);
    return retired;
}

bool RealtimeGraphHost::render(float* const* outputChannels, std::size_t channelCount, std::size_t frameCount) noexcept
{
    return render({}, outputChannels, channelCount, frameCount);
}

bool RealtimeGraphHost::render(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount) noexcept
{
    return renderPrepared(input, outputChannels, channelCount, frameCount, nullptr, nullptr);
}

RealtimeTerminalRenderResult RealtimeGraphHost::renderTerminalEvents(
    std::size_t frameCount,
    RealtimeTerminalOutputBlock& output) noexcept
{
    return renderPreparedTerminalEvents(
        frameCount, output, nullptr, nullptr);
}

RealtimeTerminalRenderResult RealtimeGraphHost::renderPreparedTerminalEvents(
    std::size_t frameCount,
    RealtimeTerminalOutputBlock& output,
    void* context,
    RealtimeGraphPreparationFn prepare) noexcept
{
    RenderMetricsGuard metrics{
        renderNonFailureCount_,
        renderFailureCount_};
    prepareNextRenderBoundary();
    adoptTransportAtAudioBoundary(true);
    audioBoundaryPrepared_ = false;
    transportBoundaryPrepared_ = false;

    auto* active = active_.load(std::memory_order_acquire);
    if (active == nullptr)
    {
        publishTransportSnapshot();
        return RealtimeTerminalRenderResult::InvalidArguments;
    }
    if (activeTransition_.load(std::memory_order_acquire) != nullptr)
    {
        publishTransportSnapshot();
        return RealtimeTerminalRenderResult::WrongRenderKind;
    }

    const auto playing = playing_.load(std::memory_order_acquire);
    const auto callbackStart = absoluteTransportFramePosition_;
    active->setTransportSnapshotBeforeRender({
        .epoch = transportEpoch_,
        .absoluteFrame = callbackStart,
        .playheadFrame = loopedTransportFrame(
            callbackStart, activeTransportLoop_),
        .sampleRate = activeTransportSampleRate_,
        .loopStartFrame = activeTransportLoop_.has_value()
            ? activeTransportLoop_->startFrame : 0,
        .loopEndFrame = activeTransportLoop_.has_value()
            ? activeTransportLoop_->endFrame : 0,
        .discontinuitySequence = transportDiscontinuitySequence_,
        .playing = playing,
        .loopEnabled = activeTransportLoop_.has_value(),
    });
    if (prepare != nullptr)
        prepare(context, *active);
    active->setPlaying(playing);
    const auto result = active->renderTerminalEvents(frameCount, output);
    activeOutputChannelCount_.store(
        active->outputChannelCount(), std::memory_order_release);
    if (playing
        && result == RealtimeTerminalRenderResult::Rendered)
    {
        if (frameCount
            > std::numeric_limits<std::uint64_t>::max()
                - absoluteTransportFramePosition_)
        {
            absoluteTransportFramePosition_ =
                std::numeric_limits<std::uint64_t>::max();
        }
        else
        {
            absoluteTransportFramePosition_ += frameCount;
        }
    }
    publishTransportSnapshot();
    metrics.succeeded = result == RealtimeTerminalRenderResult::Rendered;
    return result;
}

bool RealtimeGraphHost::renderPrepared(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    void* context,
    RealtimeGraphPreparationFn prepare) noexcept
{
    return renderPreparedInternal(
        input,
        outputChannels,
        channelCount,
        frameCount,
        context,
        prepare,
        nullptr,
        false);
}

bool RealtimeGraphHost::renderPreparedOrdered(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    void* context,
    RealtimeGraphPreparationFn prepare) noexcept
{
    return renderPreparedInternal(
        input,
        outputChannels,
        channelCount,
        frameCount,
        context,
        prepare,
        nullptr,
        true);
}

bool RealtimeGraphHost::renderPreparedMirrored(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    void* context,
    RealtimeGraphMirroredPreparationFn prepare) noexcept
{
    return renderPreparedInternal(
        input,
        outputChannels,
        channelCount,
        frameCount,
        context,
        nullptr,
        prepare,
        false);
}

bool RealtimeGraphHost::renderPreparedOrderedMirrored(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    void* context,
    RealtimeGraphMirroredPreparationFn prepare) noexcept
{
    return renderPreparedInternal(
        input,
        outputChannels,
        channelCount,
        frameCount,
        context,
        nullptr,
        prepare,
        true);
}

void RealtimeGraphHost::prepareNextRenderBoundary() noexcept
{
    if (audioBoundaryPrepared_)
        return;
    adoptPendingAtAudioBoundary();
    audioBoundaryPrepared_ = true;
}

RealtimeTransportSnapshot RealtimeGraphHost::prepareNextRenderTransportBoundary() noexcept
{
    prepareNextRenderBoundary();
    adoptTransportAtAudioBoundary(true);
    return preparedTransportSnapshot();
}

RealtimeTransportSnapshot RealtimeGraphHost::prepareContinuationRenderTransportBoundary() noexcept
{
    if (const auto target = pendingAdoptionFrame();
        target.has_value() && *target == absoluteTransportFramePosition_)
    {
        adoptPendingAtAudioBoundary();
    }
    audioBoundaryPrepared_ = true;
    adoptTransportAtAudioBoundary(false);
    return preparedTransportSnapshot();
}

void RealtimeGraphHost::adoptTransportAtAudioBoundary(bool allowControlReset) noexcept
{
    if (transportBoundaryPrepared_)
        return;
    const auto resetGeneration = allowControlReset
        ? transportResetGeneration_.load(std::memory_order_acquire)
        : adoptedTransportResetGeneration_;

    const auto hasReset = resetGeneration != adoptedTransportResetGeneration_;
    const auto hasRelocation = audioThreadRelocationPending_;
    auto relocationAccepted = hasRelocation;
    if (hasRelocation && audioThreadRelocationExpectedStageId_ != 0)
    {
        auto* renderTarget = transitioning_.load(std::memory_order_acquire);
        if (renderTarget == nullptr)
            renderTarget = active_.load(std::memory_order_acquire);
        relocationAccepted = renderTarget != nullptr &&
                             renderTarget->stageId() == audioThreadRelocationExpectedStageId_;
        if (!relocationAccepted)
        {
            addSaturatedSingleWriter(
                rejectedStageBoundRelocationCount_,
                std::max<std::uint64_t>(audioThreadRelocationSongPositionCount_, 1));
        }
    }
    if (hasReset || relocationAccepted)
    {
        absoluteTransportFramePosition_ = relocationAccepted ? audioThreadRelocationFrame_ : 0;
        incrementSaturated(transportDiscontinuitySequence_);
        if (auto* active = active_.load(std::memory_order_acquire))
            active->resetTransport();
        if (auto* transitioning = transitioning_.load(std::memory_order_acquire))
        {
            transitioning->resetTransport();
        }
        if (relocationAccepted && audioThreadRelocationExpectedStageId_ != 0)
        {
            stageBoundRelocationGeneration_.fetch_add(1, std::memory_order_acq_rel);
            addSaturatedSingleWriter(
                acceptedStageBoundRelocationCount_,
                std::max<std::uint64_t>(audioThreadRelocationSongPositionCount_, 1));
            lastStageBoundRelocationSongPosition_.store(audioThreadRelocationSongPosition_,
                                                        std::memory_order_release);
            lastStageBoundRelocationFrame_.store(audioThreadRelocationFrame_,
                                                 std::memory_order_release);
            stageBoundRelocationGeneration_.fetch_add(1, std::memory_order_release);
        }
    }
    adoptedTransportResetGeneration_ = resetGeneration;
    audioThreadRelocationExpectedStageId_ = 0;
    audioThreadRelocationSongPosition_ = 0;
    audioThreadRelocationSongPositionCount_ = 0;
    audioThreadRelocationPending_ = false;
    transportBoundaryPrepared_ = true;
}

double RealtimeGraphHost::preparedRenderSampleRate() const noexcept
{
    return activeTransportSampleRate_;
}

RealtimeTransportSnapshot RealtimeGraphHost::preparedTransportSnapshot() const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    if (source == nullptr)
        return {};
    return {
        .epoch = transportEpoch_,
        .absoluteFrame = absoluteTransportFramePosition_,
        .playheadFrame =
            loopedTransportFrame(absoluteTransportFramePosition_, activeTransportLoop_),
        .sampleRate = activeTransportSampleRate_,
        .loopStartFrame = activeTransportLoop_.has_value() ? activeTransportLoop_->startFrame : 0,
        .loopEndFrame = activeTransportLoop_.has_value() ? activeTransportLoop_->endFrame : 0,
        .discontinuitySequence = transportDiscontinuitySequence_,
        .playing = playing_.load(std::memory_order_acquire),
        .loopEnabled = activeTransportLoop_.has_value(),
    };
}

const PreparedLiveSet* RealtimeGraphHost::preparedLiveSet() const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr ? source->preparedLiveSet() : nullptr;
}

const PreparedLiveTools* RealtimeGraphHost::preparedLiveTools() const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr ? source->preparedLiveTools() : nullptr;
}

bool RealtimeGraphHost::activatePreparedLiveSetProgram(std::size_t cellIndex,
                                                       std::uint64_t transitionFrames) noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    if (source == nullptr || !source->activatePreparedLiveSetProgram(
                                 cellIndex, absoluteTransportFramePosition_, transitionFrames))
    {
        return false;
    }
    activeOutputChannelCount_.store(source->outputChannelCount(), std::memory_order_release);
    return true;
}

std::uint32_t RealtimeGraphHost::activePreparedLiveSetCellIndexPlusOne() const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr ? source->activePreparedLiveSetCellIndexPlusOne() : 0;
}

std::uint32_t RealtimeGraphHost::activePreparedLiveSetSoundCellIndexPlusOne(
    std::uint32_t column) const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr
        ? source->activePreparedLiveSetSoundCellIndexPlusOne(column) : 0;
}

bool RealtimeGraphHost::setPreparedLiveSetSoundColumnMuted(
    std::uint32_t column, bool muted) noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr
        && source->setPreparedLiveSetSoundColumnMuted(column, muted);
}

bool RealtimeGraphHost::preparedLiveSetSoundColumnMuted(
    std::uint32_t column) const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr
        && source->preparedLiveSetSoundColumnMuted(column);
}

bool RealtimeGraphHost::preparedLiveSetActivationPending() const noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    return source != nullptr && source->preparedLiveSetActivationPending();
}

void RealtimeGraphHost::deactivatePreparedLiveSetProgram() noexcept
{
    auto* source = transitioning_.load(std::memory_order_acquire);
    if (source == nullptr)
        source = active_.load(std::memory_order_acquire);
    if (source == nullptr)
        return;
    source->deactivatePreparedLiveSetProgram(absoluteTransportFramePosition_);
    activeOutputChannelCount_.store(source->outputChannelCount(), std::memory_order_release);
}

std::optional<std::uint64_t> RealtimeGraphHost::pendingAdoptionFrame() const noexcept
{
    const auto* pending = pending_.load(std::memory_order_acquire);
    return pending != nullptr ? pending->preparedStageTransportFrame() : std::nullopt;
}

std::uint64_t RealtimeGraphHost::pendingStageId() const noexcept
{
    const auto* pending = pending_.load(std::memory_order_acquire);
    return pending != nullptr ? pending->stageId() : 0;
}

void RealtimeGraphHost::adoptPendingAtAudioBoundary() noexcept
{
    auto* pending = pending_.load(std::memory_order_acquire);
    if (pending == nullptr)
        return;
    if (!pending->canAdoptAtTransportFrame(absoluteTransportFramePosition_))
    {
        const auto target = pending->preparedStageTransportFrame();
        if (target.has_value() && absoluteTransportFramePosition_ < *target
            && playing_.load(std::memory_order_acquire))
        {
            return;
        }
        // Waiting cannot recover the exact prepared source history. Retire
        // the stale candidate without touching the incumbent's DSP or cursor.
        // Publish retirement before freeing the pending slot; reclamation stays
        // on the single control producer, just like successful replacements.
        const auto rejectedStageId = pending->stageId();
        auto* transition = pendingTransition_.load(std::memory_order_acquire);
        healthSource_.store(active_.load(std::memory_order_acquire), std::memory_order_release);
        addSaturatedSingleWriter(preparedStageRejectionCount_, 1);
        if (transition != nullptr && transition->next.get() == pending)
        {
            pendingTransition_.store(nullptr, std::memory_order_release);
            retiredTransition_.store(transition, std::memory_order_release);
        }
        else
        {
            retired_.store(pending, std::memory_order_release);
        }
        pending_.store(nullptr, std::memory_order_release);
        lastRejectedPreparedStageId_.store(rejectedStageId, std::memory_order_release);
        return;
    }
    const auto* currentActive = active_.load(std::memory_order_acquire);
    auto* pendingTransition =
        pendingTransition_.load(std::memory_order_acquire);
    if (pendingTransition != nullptr
        && pendingTransition->next.get() == pending
        && currentActive != nullptr)
    {
        pending->setPlaying(playing_.load(std::memory_order_acquire));
        activeTransportLoop_ = pending->transportLoop();
        activeStageId_.store(pending->stageId(), std::memory_order_release);
        activeOutputChannelCount_.store(
            pending->outputChannelCount(),
            std::memory_order_release);
        activeTransportSampleRate_ = pending->sampleRate();
        transitioning_.store(pending, std::memory_order_release);
        healthSource_.store(pending, std::memory_order_release);
        activeTransition_.store(
            pendingTransition,
            std::memory_order_release);
        pendingTransition_.store(nullptr, std::memory_order_release);
        auto* expectedPending = pending;
        pending_.compare_exchange_strong(
            expectedPending,
            nullptr,
            std::memory_order_acq_rel);
        return;
    }

    if (currentActive == nullptr && transportEpoch_ == 0)
        transportEpoch_ = 1;
    if (currentActive != nullptr
        && currentActive->sampleRate() != pending->sampleRate())
    {
        absoluteTransportFramePosition_ = 0;
        incrementSaturated(transportEpoch_);
        incrementSaturated(transportDiscontinuitySequence_);
    }
    pending->setPlaying(playing_.load(std::memory_order_acquire));
    activeTransportLoop_ = pending->transportLoop();
    auto* previous = active_.exchange(pending, std::memory_order_acq_rel);
    healthSource_.store(pending, std::memory_order_release);
    activeStageId_.store(pending->stageId(), std::memory_order_release);
    activeOutputChannelCount_.store(
        pending->outputChannelCount(),
        std::memory_order_release);
    activeTransportSampleRate_ = pending->sampleRate();
    if (previous != nullptr)
        retired_.store(previous, std::memory_order_release);
    auto* expectedPending = pending;
    pending_.compare_exchange_strong(
        expectedPending,
        nullptr,
        std::memory_order_acq_rel);
}

bool RealtimeGraphHost::renderPreparedInternal(
    RealtimeAudioInputBlock input,
    float* const* outputChannels,
    std::size_t channelCount,
    std::size_t frameCount,
    void* context,
    RealtimeGraphPreparationFn prepare,
    RealtimeGraphMirroredPreparationFn mirroredPrepare,
    bool orderedOutputs) noexcept
{
    RenderMetricsGuard metrics{
        renderNonFailureCount_,
        renderFailureCount_};
    prepareNextRenderBoundary();

    adoptTransportAtAudioBoundary(true);
    audioBoundaryPrepared_ = false;

    transportBoundaryPrepared_ = false;

    auto* active = active_.load(std::memory_order_acquire);
    if (active == nullptr)
    {
        publishTransportSnapshot();
        clearOutputChannels(outputChannels, channelCount, frameCount);
        return false;
    }

    const auto callbackStart = absoluteTransportFramePosition_;
    const auto playing = playing_.load(std::memory_order_acquire);
    const RealtimeTransportSnapshot callbackSnapshot{
        .epoch = transportEpoch_,
        .absoluteFrame = callbackStart,
        .playheadFrame = loopedTransportFrame(callbackStart, activeTransportLoop_),
        .sampleRate = activeTransportSampleRate_,
        .loopStartFrame = activeTransportLoop_.has_value()
            ? activeTransportLoop_->startFrame : 0,
        .loopEndFrame = activeTransportLoop_.has_value()
            ? activeTransportLoop_->endFrame : 0,
        .discontinuitySequence = transportDiscontinuitySequence_,
        .playing = playing,
        .loopEnabled = activeTransportLoop_.has_value(),
    };
    if (auto* transition =
            activeTransition_.load(std::memory_order_acquire))
    {
        auto* candidate = transition->next.get();
        if (candidate == nullptr
            || frameCount > candidate->maxBlockFrames()
            || transition->oldPointers.size()
                != transition->newPointers.size())
        {
            clearOutputChannels(
                outputChannels,
                channelCount,
                frameCount);
            publishTransportSnapshot();
            return false;
        }
        active->setTransportSnapshotBeforeRender(callbackSnapshot);
        candidate->setTransportSnapshotBeforeRender(callbackSnapshot);
        if (mirroredPrepare != nullptr)
            mirroredPrepare(context, *candidate, active);
        else if (prepare != nullptr)
            prepare(context, *candidate);
        active->setPlaying(playing);
        candidate->setPlaying(playing);
        if (playing)
        {
            std::uint64_t missingChannels = 0;
            for (std::size_t channel = 0;
                 channel < candidate->outputChannelCount();
                 ++channel)
            {
                if (outputChannels == nullptr
                    || channel >= channelCount
                    || outputChannels[channel] == nullptr)
                {
                    ++missingChannels;
                }
            }
            if (missingChannels != 0)
            {
                addSaturatedSingleWriter(
                    discardedOutputChannelCount_,
                    missingChannels);
            }
        }
        const auto scratchChannelCount =
            transition->newPointers.size();
        const auto oldRendered = orderedOutputs
            ? active->renderOrdered(
                  input,
                  transition->oldPointers.data(),
                  scratchChannelCount,
                  frameCount)
            : active->render(
                  input,
                  transition->oldPointers.data(),
                  scratchChannelCount,
                  frameCount);
        const auto newRendered = orderedOutputs
            ? candidate->renderOrdered(
                  input,
                  transition->newPointers.data(),
                  scratchChannelCount,
                  frameCount)
            : candidate->render(
                  input,
                  transition->newPointers.data(),
                  scratchChannelCount,
                  frameCount);
        activeOutputChannelCount_.store(candidate->outputChannelCount(), std::memory_order_release);
        if (outputChannels != nullptr)
        {
            for (std::size_t channel = 0;
                 channel < channelCount;
                 ++channel)
            {
                auto* output = outputChannels[channel];
                if (output == nullptr)
                    continue;
                if (channel >= scratchChannelCount)
                {
                    std::fill_n(output, frameCount, 0.0F);
                    continue;
                }
                for (std::size_t frame = 0;
                     frame < frameCount;
                     ++frame)
                {
                    const auto progressed =
                        transition->completedFrames
                        > std::numeric_limits<std::uint64_t>::max()
                                - static_cast<std::uint64_t>(frame + 1)
                            ? std::numeric_limits<std::uint64_t>::max()
                            : transition->completedFrames
                                + static_cast<std::uint64_t>(frame + 1);
                    const auto alpha = static_cast<float>(
                        std::min(progressed, transition->totalFrames))
                        / static_cast<float>(transition->totalFrames);
                    output[frame] =
                        transition->oldScratch[channel][frame]
                            * (1.0F - alpha)
                        + transition->newScratch[channel][frame]
                            * alpha;
                }
            }
        }
        const auto remaining = transition->totalFrames
            - std::min(
                transition->completedFrames,
                transition->totalFrames);
        transition->completedFrames += std::min<std::uint64_t>(
            remaining,
            frameCount);
        if (transition->completedFrames >= transition->totalFrames)
        {
            auto* adopted = transition->next.release();
            auto* previous = active_.exchange(
                adopted,
                std::memory_order_acq_rel);
            transitioning_.store(nullptr, std::memory_order_release);
            retiredTransition_.store(
                transition,
                std::memory_order_release);
            retired_.store(previous, std::memory_order_release);
            activeTransition_.store(nullptr, std::memory_order_release);
            addSaturatedSingleWriter(transitionCompletionToken_, 1);
        }
        if (playing)
        {
            if (frameCount
                > std::numeric_limits<std::uint64_t>::max()
                    - absoluteTransportFramePosition_)
            {
                absoluteTransportFramePosition_ =
                    std::numeric_limits<std::uint64_t>::max();
            }
            else
            {
                absoluteTransportFramePosition_ += frameCount;
            }
        }
        publishTransportSnapshot();
        const auto rendered = oldRendered && newRendered;
        metrics.succeeded = !playing || rendered;
        return rendered;
    }
    active->setTransportSnapshotBeforeRender(callbackSnapshot);
    if (mirroredPrepare != nullptr)
        mirroredPrepare(context, *active, nullptr);
    else if (prepare != nullptr)
        prepare(context, *active);

    active->setPlaying(playing);
    if (playing)
    {
        std::uint64_t missingChannels = 0;
        for (std::size_t channel = 0; channel < active->outputChannelCount(); ++channel)
        {
            if (outputChannels == nullptr || channel >= channelCount
                || outputChannels[channel] == nullptr)
            {
                ++missingChannels;
            }
        }
        if (missingChannels != 0)
            addSaturatedSingleWriter(
                discardedOutputChannelCount_, missingChannels);
    }
    const auto rendered = orderedOutputs
        ? active->renderOrdered(
              input,
              outputChannels,
              channelCount,
              frameCount)
        : active->render(
              input,
              outputChannels,
              channelCount,
              frameCount);
    activeOutputChannelCount_.store(active->outputChannelCount(), std::memory_order_release);
    if (playing)
    {
        if (frameCount
            > std::numeric_limits<std::uint64_t>::max()
                - absoluteTransportFramePosition_)
        {
            absoluteTransportFramePosition_ =
                std::numeric_limits<std::uint64_t>::max();
        }
        else
        {
            absoluteTransportFramePosition_ += frameCount;
        }
    }
    publishTransportSnapshot();
    metrics.succeeded = !playing || rendered;
    return rendered;
}

void RealtimeGraphHost::clearAfterAudioStopped() noexcept
{
    audioBoundaryPrepared_ = false;
    transportBoundaryPrepared_ = false;
    healthSource_.store(nullptr, std::memory_order_release);
    auto* active = active_.exchange(nullptr, std::memory_order_acq_rel);
    activeOutputChannelCount_.store(0, std::memory_order_release);
    activeStageId_.store(0, std::memory_order_release);
    lastRejectedPreparedStageId_.store(0, std::memory_order_release);
    auto* pending = pending_.exchange(nullptr, std::memory_order_acq_rel);
    auto* retired = retired_.exchange(nullptr, std::memory_order_acq_rel);
    auto* pendingTransition = pendingTransition_.exchange(
        nullptr,
        std::memory_order_acq_rel);
    auto* activeTransition = activeTransition_.exchange(
        nullptr,
        std::memory_order_acq_rel);
    auto* retiredTransition = retiredTransition_.exchange(
        nullptr,
        std::memory_order_acq_rel);
    transitioning_.store(nullptr, std::memory_order_release);
    const auto hadTransportEpoch = active != nullptr
        || pending != nullptr
        || retired != nullptr
        || activeTransportSampleRate_ > 0.0;
    delete active;
    if (pendingTransition == nullptr)
        delete pending;
    delete retired;
    delete pendingTransition;
    delete activeTransition;
    delete retiredTransition;
    if (hadTransportEpoch)
    {
        incrementSaturated(transportEpoch_);
        incrementSaturated(transportDiscontinuitySequence_);
    }
    absoluteTransportFramePosition_ = 0;
    activeTransportSampleRate_ = 0.0;
    activeTransportLoop_.reset();
    clearExternalMusicalTick();
    clearExternalMtc();
    publishTransportSnapshot();
    adoptedTransportResetGeneration_ = transportResetGeneration_.load(std::memory_order_acquire);
}
}
