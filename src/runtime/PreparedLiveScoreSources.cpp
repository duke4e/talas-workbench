#include "PreparedLiveScoreSources.h"
#include "ScoreTrackPanning.h"
#include "PreparedScoreClipSource.h"
#include "talas/runtime/AudioStream.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace talas::runtime
{
std::shared_ptr<PreparedLiveScoreSources> prepareLiveScoreSources(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry,
    const RenderSettings& settings, std::string& error)
{
    if (!settings.preparedScore) { error = "Live Score requires a prepared schedule"; return {}; }
    if ((!settings.enableRealtimeAnalysisCapture && !settings.realtimeAnalysisTargets.empty())
        || settings.realtimeAnalysisTargets.size() > maximumRealtimeAnalysisProbeCount)
    { error = "Live Score has invalid analysis target settings"; return {}; }
    std::vector<bool> resolvedTargets(settings.realtimeAnalysisTargets.size());
    auto result = std::make_shared<PreparedLiveScoreSources>();
    result->schedule = settings.preparedScore;
    // These bound aggregate graph work, not an exhaustive DSP storage estimate.
    // Keep memory-accounting expansion explicit before raising the live profile.
    constexpr std::size_t maximumSources = 64, maximumOperations = 4096;
    std::size_t operations = 0, audioValues = 0;
    for (std::size_t c = 0; c < result->schedule->clips().size(); ++c)
    {
        const auto& clip = result->schedule->clips()[c];
        if (!clip.audible || (settings.scorePlaybackEndFrameExclusive
            && clip.startFrame >= *settings.scorePlaybackEndFrameExclusive)) continue;
        if (std::ranges::any_of(settings.preparedScoreClipAudio,
            [&](const auto& audio) { return audio && audio->clipId == clip.clipId; })) continue;
        if (clip.repeatCount > maximumSources - result->sources.size())
        { error = "Live Score exceeds its prepared source-instance budget"; return {}; }
        const auto channels = result->schedule->tracks()[clip.trackIndex].channelCount;
        const auto buffers = std::size_t(channels) * (std::size_t(clip.repeatCount) + 1);
        if (channels == 0 || channels > 8 || settings.maxBlockFrames > (1U << 21) / buffers
            || audioValues > (1U << 21) - settings.maxBlockFrames * buffers)
        { error = "Live Score exceeds its callback audio storage budget"; return {}; }
        audioValues += settings.maxBlockFrames * buffers;
        if (clip.contentOffsetFrames > 1000000)
        { error = "Live Score exceeds its source offset preparation budget"; return {}; }
        for (std::uint32_t repeat = 0; repeat < clip.repeatCount; ++repeat)
        {
            std::vector<std::size_t> preparedTargets;
            auto compiled = prepareScoreClipSourceIteration(document, registry, settings, c, repeat, &preparedTargets);
            if (!compiled.ok())
            {
                error = "Live Score source compilation failed for '" + clip.clipId + "'";
                for (const auto& diagnostic : compiled.diagnostics) error += " | " + diagnostic.message;
                return {};
            }
            if (compiled.graph->plan().steps.size() > maximumOperations - operations)
            { error = "Live Score exceeds its aggregate source operation budget"; return {}; }
            operations += compiled.graph->plan().steps.size();
            for (const auto target : preparedTargets) resolvedTargets[target] = true;
            if (auto capture = compiled.graph->analysisCapture())
            {
                if (!result->analysisSources) result->analysisSources = std::make_shared<PreparedScoreSourceAnalysisSet>();
                result->analysisSources->push_back({clip.clipId, repeat, clip.soundDefinitionId,
                    clip.sourceInstanceId, std::move(capture)});
            }
            std::vector<std::size_t> timelineWaitSteps;
            std::optional<RuntimeScopeId> primaryOwner;
            const std::vector<std::string> primaryPath{clip.sourceInstanceId.value_or("scoreMaterializationSource")};
            for (const auto& scope : compiled.graph->preparedVariables().scopes)
                if (scope.definitionId == clip.soundDefinitionId && scope.instancePath == primaryPath
                    && std::ranges::any_of(compiled.graph->plan().steps, [&](const auto& step) {
                        return step.structuralScopeId == scope.scopeOrdinal;
                    }))
                {
                    if (primaryOwner) { error = "Timeline clip has ambiguous primary clock owner"; return {}; }
                    primaryOwner = scope.scopeOrdinal;
                }
            for (std::size_t step = 0; step < compiled.graph->plan().steps.size(); ++step)
            {
                const auto& operation = compiled.graph->plan().steps[step];
                if (operation.typeId == "talas.waitUntil" && primaryOwner
                    && operation.structuralScopeId == *primaryOwner && operation.scopeId == *primaryOwner)
                    timelineWaitSteps.push_back(step);
            }
            result->sources.push_back({c, result->schedule->clipIterations()[clip.firstIteration + repeat],
                std::move(*compiled.graph), std::move(timelineWaitSteps)});
        }
    }
    if (std::ranges::any_of(resolvedTargets, [](bool resolved) { return !resolved; }))
    { error = "Score analysis target does not resolve to an audible live clip source"; return {}; }
    return result;
}
PreparedLiveScorePlayback::PreparedLiveScorePlayback(std::shared_ptr<const PreparedLiveScoreSources> authority)
    : authority_(std::move(authority))
{
    if (!authority_ || !authority_->schedule) { error_ = "Live Score source authority is missing"; return; }
    const auto maxFrames = authority_->parentSettings.maxBlockFrames;
    audio_.resize(authority_->schedule->clips().size());
    // Charge retained source windows across every repeat, in addition to the
    // parent/source callback audio budget checked during compilation.
    for (const auto& prepared : authority_->sources)
    {
        const auto& clip = authority_->schedule->clips()[prepared.clipIndex];
        const auto channels = authority_->schedule->tracks()[clip.trackIndex].channelCount;
        auto& audio = audio_[prepared.clipIndex];
        if (audio.empty()) audio.assign(channels, std::vector<double>(maxFrames));
        SourceState state;
        state.callback = std::make_unique<PreparedSourceCallback>(prepared.graph);
        if (!state.callback->ready()) { error_ = state.callback->error(); return; }
        if (!state.callback->bindTimelineWaitSteps(prepared.timelineWaitSteps))
        { error_ = "Timeline clip has invalid gate ownership"; return; }
        hasTimelineGates_ = hasTimelineGates_ || !prepared.timelineWaitSteps.empty();
        state.scratch.assign(channels, std::vector<double>(maxFrames));
        // Source offset history is prepared with the explicit offline input
        // snapshot (silence if absent). New device input is read only in begin().
        std::array<std::vector<double>, 8> inputScratch;
        std::array<const double*, 8> inputs{};
        std::array<double*, 8> outputs{};
        for (std::size_t ch = 0; ch < 8; ++ch) { inputScratch[ch].resize(maxFrames); inputs[ch] = inputScratch[ch].data(); }
        for (std::size_t ch = 0; ch < channels; ++ch) outputs[ch] = state.scratch[ch].data();
        // Offset history must also be independent of worker progress. This
        // checkpoint and source decode are constructed on the control thread.
        auto offsetTransaction = clip.contentOffsetFrames ? state.callback->beginOfflineTransaction() : nullptr;
        if (clip.contentOffsetFrames && !offsetTransaction)
        { error_ = "Live Score source offset transaction preparation failed"; return; }
        while (state.position < clip.contentOffsetFrames)
        {
            const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(maxFrames, clip.contentOffsetFrames - state.position));
            for (auto& ch : inputScratch) std::fill(ch.begin(), ch.end(), 0);
            const auto hardware = authority_->schedule->tracks()[clip.trackIndex].inputChannel;
            for (const auto& ch : prepared.graph.renderSettings().audioInputChannels)
            {
                // A track's hardware input as
                // the Sound's first input, on each of its channels.
                for (std::size_t to = 0; to < 8; ++to)
                {
                    const auto from = hardware == 0 ? to + 1 : to < channels ? hardware : 0;
                    if (static_cast<std::size_t>(ch.channel) != from) continue;
                    for (std::size_t f = 0; f < frames && state.position + f < ch.samples.size(); ++f)
                        inputScratch[to][f] = ch.samples[state.position + f];
                }
            }
            if (!state.callback->renderTentative({nullptr, inputs.size(), inputs.data()}, outputs.data(), channels, frames))
            { error_ = "Live Score source offset preparation failed"; return; }
            state.callback->commit(); state.position += frames;
        }
        if (offsetTransaction && !offsetTransaction->commit())
        { error_ = "Live Score source offset transaction commit failed"; return; }
        if (!state.callback->prepareStreamWindows(streamWindowBudget_))
        { error_ = "Live Score exceeds its prepared source stream window budget"; return; }
        if (!state.callback->prepareResetPoint())
        { error_ = "Live Score source reset point preparation failed"; return; }
        state.midiChannels = state.callback->midiChannels();
        midiChannels_ |= state.midiChannels;
        // The global Event Values it follows live,
        // not those the Timeline automates for it, nor the keyboard's, which
        // follow its notes, nor the tempo its clip gives it.
        const auto& automated = prepared.graph.renderSettings().eventValueChanges;
        for (const auto& value : state.callback->globalEventValues())
        {
            if (value.name == "KeyDown" || value.name == "Pitch" || value.name == "KeyVelocity" || value.name == "BPM"
                || std::ranges::any_of(automated, [&](const EventValueChange& change) { return change.name == value.name; }))
                continue;
            state.followed.push_back({value.name, value.domain});
        }
        states_.push_back(std::move(state));
    }
    midiOutput_.resize(authority_->parentSettings.maxRealtimeTerminalEvents);
}
PreparedLiveScorePlayback::~PreparedLiveScorePlayback() { rollback(); }
bool PreparedLiveScorePlayback::prepareTransportPoint(std::uint64_t frame, std::string& error)
{
    error.clear();
    if (!ready() || hasBegunRender_ || callbackActive_ || offline_)
    { error = "Live Score transport preparation requires an unpublished, unrendered source owner"; return false; }
    if (preparedTransportFrame_)
    {
        if (*preparedTransportFrame_ == frame) return true;
        error = "This source owner already has a different prepared transport point"; return false;
    }
    std::uint64_t workFrames = 0;
    for (const auto& source : authority_->sources)
        if (frame >= source.iteration.startFrame && frame < source.iteration.endFrame)
        {
            const auto age = frame - source.iteration.startFrame;
            if (age > 1000000 - workFrames)
            { error = "Live Score transport reconstruction exceeds its aggregate preparation budget"; return false; }
            workFrames += age;
        }
    for (const auto& source : states_)
        if (!source.callback->canResetToPreparedPoint())
        { error = "Live Score initial source state is unavailable"; return false; }
    // Keep every active source's full offline journal until the whole candidate
    // is prepared. On failure this restores DSP, stream generations/windows and
    // preparation budget, including sources that succeeded before a later one.
    struct BudgetRollback
    {
        AudioStreamPreparedWindowBudget& live;
        AudioStreamPreparedWindowBudget initial;
        bool committed = false;
        ~BudgetRollback() { if (!committed) live = initial; }
    } budgetRollback{streamWindowBudget_, streamWindowBudget_};
    std::vector<std::unique_ptr<RealtimeRenderSession::OfflineTransaction>> transactions;
    transactions.reserve(states_.size());
    std::vector<std::unique_ptr<PreparedSourceCallback::PreparedPoint>> points;
    std::vector<std::uint64_t> positions;
    points.reserve(states_.size()); positions.reserve(states_.size());
    const auto maxFrames = authority_->parentSettings.maxBlockFrames;
    std::array<std::vector<double>, 8> inputScratch;
    std::array<const double*, 8> inputs{};
    for (std::size_t ch = 0; ch < inputs.size(); ++ch)
    { inputScratch[ch].resize(maxFrames); inputs[ch] = inputScratch[ch].data(); }
    for (std::size_t index = 0; index < states_.size(); ++index)
    {
        auto& state = states_[index];
        const auto& prepared = authority_->sources[index];
        const auto& clip = authority_->schedule->clips()[prepared.clipIndex];
        const auto age = frame >= prepared.iteration.startFrame && frame < prepared.iteration.endFrame
            ? frame - prepared.iteration.startFrame : 0;
        if (age > std::numeric_limits<std::uint64_t>::max() - clip.contentOffsetFrames)
        { error = "Live Score target source position overflows"; return false; }
        const auto target = clip.contentOffsetFrames + age;
        auto transaction = age ? state.callback->beginOfflineTransaction() : nullptr;
        if (age && !transaction)
        { error = "Live Score target history transaction is unavailable"; return false; }
        std::array<double*, 8> outputs{};
        for (std::size_t ch = 0; ch < state.scratch.size(); ++ch) outputs[ch] = state.scratch[ch].data();
        for (auto position = clip.contentOffsetFrames; position < target;)
        {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(maxFrames, target - position));
            for (auto& channel : inputScratch) std::fill(channel.begin(), channel.end(), 0);
            for (const auto& channel : prepared.graph.renderSettings().audioInputChannels)
                if (channel.channel >= 1 && channel.channel <= 8)
                    for (std::size_t f = 0; f < count && position + f < channel.samples.size(); ++f)
                        inputScratch[channel.channel - 1][f] = channel.samples[position + f];
            if (!state.callback->renderTentative({nullptr, inputs.size(), inputs.data()}, outputs.data(), state.scratch.size(), count))
            { error = "Live Score target source history failed"; return false; }
            state.callback->commit(); position += count;
        }
        if (age && !state.callback->prepareStreamWindows(streamWindowBudget_))
        { error = "Live Score target exceeds its prepared stream window budget"; return false; }
        auto point = state.callback->capturePreparedPoint();
        if (!point) { error = "Live Score target source checkpoint is unavailable"; return false; }
        if (age && !state.callback->resetToPreparedPoint())
        { error = "Live Score target preparation could not restore its initial source state"; return false; }
        transactions.push_back(std::move(transaction));
        points.push_back(std::move(point)); positions.push_back(target);
    }
    // Private live sources exclude nested Score and isolated/external commit
    // participants; these commits only retain prepared resources and reset DSP.
    for (auto& transaction : transactions)
        if (transaction && !transaction->commit())
        { error = "Live Score target preparation commit failed"; return false; }
    budgetRollback.committed = true;
    transportPoints_ = std::move(points); transportPositions_ = std::move(positions);
    preparedTransportFrame_ = frame;
    return true;
}
bool PreparedLiveScorePlayback::canRelocatePrepared(std::uint64_t frame) const noexcept
{
    if (!ready() || callbackActive_) return false;
    if (preparedTransportFrame_ == frame)
    {
        for (std::size_t index = 0; index < states_.size(); ++index)
            if (!states_[index].callback->canRestorePreparedPoint(*transportPoints_[index])) return false;
        return true;
    }
    // Inactive clips and a clip's exact start need only their initial reset point.
    for (const auto& source : authority_->sources)
        if (frame > source.iteration.startFrame && frame < source.iteration.endFrame) return false;
    for (const auto& state : states_) if (!state.callback->canResetToPreparedPoint()) return false;
    return true;
}
bool PreparedLiveScorePlayback::relocatePrepared(std::uint64_t frame) noexcept
{
    if (!canRelocatePrepared(frame)) return false;
    if (preparedTransportFrame_ == frame)
    {
        for (std::size_t index = 0; index < states_.size(); ++index)
        {
            if (!states_[index].callback->restorePreparedPoint(*transportPoints_[index]))
            { positioned_ = false; return false; }
            states_[index].position = transportPositions_[index];
            states_[index].pendingFrames = states_[index].pendingCompositionFrames = states_[index].lastCommittedFrames = 0;
        }
        restartFollowedValues();
        positioned_ = true;
        return true;
    }
    reset(); return positioned_;
}
bool PreparedLiveScorePlayback::begin(std::uint64_t playhead, std::size_t frames) noexcept
{
    return beginSegment(playhead, frames, 0);
}
bool PreparedLiveScorePlayback::beginSegment(
    std::uint64_t playhead, std::size_t frames, std::size_t callbackOffset) noexcept
{
    if (!ready() || !positioned_ || callbackActive_ || frames == 0
        || frames > authority_->parentSettings.maxBlockFrames
        || callbackOffset >= frames) return false;
    for (const auto& state : states_) if (state.callback->pending()) return false;
    for (auto& clip : audio_) for (auto& channel : clip) std::fill_n(channel.data(), frames, 0.0);
    timelineDecisionPending_ = false; preparingTimelineFrame_ = false;
    // A loop's second segment keeps what the first sent.
    if (callbackOffset == 0) midiOutputCount_ = 0;
    hasBegunRender_ = true;
    callbackActive_ = true; callbackFrames_ = frames; renderedFrames_ = callbackOffset;
    expectedPlayhead_ = playhead;
    return true;
}
bool PreparedLiveScorePlayback::sealSegment(std::size_t callbackEndOffset) noexcept
{
    if (!callbackActive_ || timelineDecisionPending_
        || renderedFrames_ != callbackEndOffset || callbackEndOffset > callbackFrames_)
        return false;
    callbackFrames_ = callbackEndOffset;
    return true;
}
bool PreparedLiveScorePlayback::renderSegment(RealtimeAudioInputBlock input, std::uint64_t playhead,
    std::size_t frames, std::size_t inputOffset, std::size_t outputOffset, bool compositionAdvancing,
    std::span<const LiveScoreMidiNote> notes) noexcept
{
    if (timelineDecisionPending_ || !callbackActive_ || playhead != expectedPlayhead_ || frames == 0
        || renderedFrames_ > callbackFrames_ || frames > callbackFrames_ - renderedFrames_
        || outputOffset != renderedFrames_ || inputOffset > callbackFrames_ - frames
        || (compositionAdvancing && frames > std::numeric_limits<std::uint64_t>::max() - playhead))
        return false;
    for (std::size_t s = 0; s < states_.size(); ++s)
    {
        const auto& source = authority_->sources[s];
        const auto& clip = authority_->schedule->clips()[source.clipIndex];
        auto& state = states_[s];
        const auto start = compositionAdvancing ? std::max(playhead, source.iteration.startFrame) : playhead;
        const auto end = compositionAdvancing ? std::min(playhead + frames, source.iteration.endFrame) : playhead;
        if (compositionAdvancing ? start >= end
            : playhead < source.iteration.startFrame || playhead >= source.iteration.endFrame) continue;
        const auto age = start - source.iteration.startFrame;
        if (age > std::numeric_limits<std::uint64_t>::max() - clip.contentOffsetFrames
            || state.pendingCompositionFrames > std::numeric_limits<std::uint64_t>::max() - state.position
            || state.position + state.pendingCompositionFrames != clip.contentOffsetFrames + age) return false;
        const auto count = compositionAdvancing ? static_cast<std::size_t>(end - start) : frames;
        const auto offset = compositionAdvancing ? static_cast<std::size_t>(start - playhead) : 0;
        std::array<const float*, 8> inputs{};
        std::array<const double*, 8> doubleInputs{};
        std::array<double*, 8> outputs{};
        // A track's hardware input is its Sound's
        // first input, on each of the track's channels.
        const auto hardware = authority_->schedule->tracks()[clip.trackIndex].inputChannel;
        for (std::size_t ch = 0; ch < std::min<std::size_t>(input.channelCount, 8); ++ch)
        {
            const auto from = hardware == 0 ? ch : ch < state.scratch.size() ? hardware - 1 : input.channelCount;
            if (from >= input.channelCount) continue;
            inputs[ch] = input.channels && input.channels[from] ? input.channels[from] + inputOffset + offset : nullptr;
            doubleInputs[ch] = input.doubleChannels && input.doubleChannels[from] ? input.doubleChannels[from] + inputOffset + offset : nullptr;
        }
        for (std::size_t ch = 0; ch < state.scratch.size(); ++ch) outputs[ch] = state.scratch[ch].data();
        const RealtimeAudioInputBlock sourceInput{inputs.data(), std::min<std::size_t>(input.channelCount, 8), doubleInputs.data()};
        // A global Event Value the parent changed
        // live, from a MIDI fader, the surface or a preset, from the Sound's
        // first frame in the callback on.
        if (!state.callback->pending() && parentValue_ != nullptr)
        {
            for (auto& followed : state.followed)
            {
                const auto parent = parentValue_(parent_, followed.name, followed.domain);
                if (!parent.has_value() || !std::isfinite(parent->current))
                    continue;
                const auto seen = std::isnan(followed.committed) ? parent->initial : followed.committed;
                if (parent->current != seen
                    && state.callback->scheduleEventValue(followed.name, parent->current, followed.domain, 0))
                    followed.pending = parent->current;
            }
        }
        // Notes is always live, from the Sound's first
        // frame in the callback on.
        if (!state.callback->pending() && state.midiChannels != 0)
        {
            const auto first = outputOffset + offset;
            for (const auto& note : notes)
            {
                if (note.callbackOffset >= first && note.event.channel >= 1 && note.event.channel <= 16
                    && ((state.midiChannels >> (note.event.channel - 1U)) & 1U) != 0)
                    (void) state.callback->scheduleMidiNote(note.event, note.callbackOffset - first);
            }
        }
        if (!state.callback->pending())
            state.midiTaken = 0;
        // The source's frames so far this callback, where this segment
        // begins in its own callback.
        const auto sourceStart = state.pendingFrames;
        const auto rendered = preparingTimelineFrame_
            ? (count == 1 && state.callback->renderTimelineFrame(sourceInput, outputs.data(), state.scratch.size()))
            : state.callback->pending()
            ? state.callback->renderNextSegment(sourceInput, outputs.data(), state.scratch.size(), count, compositionAdvancing)
            : state.callback->renderTentative(sourceInput, outputs.data(), state.scratch.size(), count, compositionAdvancing);
        if (!rendered) return false;
        // A MIDI output within the clip's Sound sends
        // through the Timeline, each event at its frame of the parent's
        // callback.
        const auto sent = state.callback->midiOutputEvents();
        for (auto index = std::min(state.midiTaken, sent.size()); index < sent.size(); ++index)
        {
            auto event = sent[index];
            if (midiOutputCount_ >= midiOutput_.size() || event.frameOffset < sourceStart
                || event.frameOffset - sourceStart >= count)
                return false;
            event.frameOffset = event.frameOffset - sourceStart + outputOffset + offset;
            midiOutput_[midiOutputCount_++] = event;
        }
        state.midiTaken = sent.size();
        state.pendingFrames += count;
        if (compositionAdvancing) state.pendingCompositionFrames += count;
        const auto replaces = authority_->schedule->tracks()[clip.trackIndex].overlapMode
            == talas::project::ScoreOverlapMode::Replace;
        // The track's Angle and Radius, with the Sound's own
        // Panner and Distance, on two channels or the placed speakers.
        const auto& speakers = authority_->parentSettings.speakerPlacement;
        const auto panned = (state.scratch.size() == 2 || !speakers.empty())
            && scoreClipIsPanned(*authority_->schedule, static_cast<std::uint32_t>(source.clipIndex), clip.trackIndex);
        // The Levels of its track and of the submixes on its way.
        const auto levelled = scoreHasTrackLevels(*authority_->schedule) || scoreHasClipFades(*authority_->schedule);
        for (std::size_t f = 0; f < count; ++f)
        {
            const auto absolute = start + (compositionAdvancing ? f : 0);
            const auto clipFrame = absolute - clip.startFrame;
            const auto remaining = clip.endFrame - absolute;
            double gain = !replaces || authority_->schedule->clipAudibleAt(source.clipIndex, absolute) ? 1 : 0;
            if (clip.fadeInFrames && clipFrame < clip.fadeInFrames) gain = double(clipFrame) / clip.fadeInFrames;
            if (clip.fadeOutFrames && remaining <= clip.fadeOutFrames) gain = std::min(gain, double(remaining - 1) / clip.fadeOutFrames);
            const auto pan = panned
                ? scoreClipChannelGains(*authority_->schedule, static_cast<std::uint32_t>(source.clipIndex), clip.trackIndex,
                      absolute, speakers, state.scratch.size())
                : std::nullopt;
            const auto level = levelled ? scoreRouteLevelAt(*authority_->schedule, clip.trackIndex, absolute)
                    * scoreClipFadeAt(*authority_->schedule, source.clipIndex, absolute)
                                        : 1.0;
            for (std::size_t ch = 0; ch < state.scratch.size(); ++ch)
                audio_[source.clipIndex][ch][outputOffset + offset + f] = state.scratch[ch][f] * std::clamp(gain, 0.0, 1.0)
                    * level * (pan.has_value() ? (ch < pan->size() ? (*pan)[ch] : 0.0) : 1.0);
        }
    }
    renderedFrames_ += frames;
    if (compositionAdvancing) expectedPlayhead_ += frames;
    return true;
}
bool PreparedLiveScorePlayback::renderTimelineFrame(RealtimeAudioInputBlock input, std::uint64_t playhead,
    std::size_t inputOffset, std::size_t outputOffset, std::span<const LiveScoreMidiNote> notes) noexcept
{
    if (!hasTimelineGates_ || timelineDecisionPending_) return false;
    preparingTimelineFrame_ = true;
    const auto rendered = renderSegment(input, playhead, 1, inputOffset, outputOffset, false, notes);
    preparingTimelineFrame_ = false;
    timelineDecisionPending_ = rendered;
    return rendered;
}
bool PreparedLiveScorePlayback::timelineHeld() const noexcept
{
    if (!timelineDecisionPending_) return false;
    return std::ranges::any_of(states_, [](const auto& state) { return state.callback->timelineHeld(); });
}
bool PreparedLiveScorePlayback::resolveTimelineFrame(bool advanceComposition) noexcept
{
    if (!timelineDecisionPending_ || (advanceComposition && expectedPlayhead_ == std::numeric_limits<std::uint64_t>::max()))
        return false;
    for (auto& state : states_)
        if (state.callback->timelineDecisionPending())
        {
            if (!state.callback->resolveTimelineFrame(advanceComposition)) return false;
            if (advanceComposition) ++state.pendingCompositionFrames;
        }
    if (advanceComposition) ++expectedPlayhead_;
    timelineDecisionPending_ = false;
    return true;
}
void PreparedLiveScorePlayback::reset() noexcept
{
    rollback(); positioned_ = true;
    for (std::size_t index = 0; index < states_.size(); ++index)
    {
        auto& state = states_[index];
        if (!state.callback->resetToPreparedPoint()) positioned_ = false;
        state.position = authority_->schedule->clips()[authority_->sources[index].clipIndex].contentOffsetFrames;
        state.pendingFrames = 0; state.lastCommittedFrames = 0;
    }
    restartFollowedValues();
}
bool PreparedLiveScorePlayback::canCommit() const noexcept
{
    return !timelineDecisionPending_ && callbackActive_ && renderedFrames_ == callbackFrames_;
}
bool PreparedLiveScorePlayback::commit() noexcept
{
    if (!canCommit()) return false;
    for (auto& state : states_)
    {
        state.callback->commit(); state.position += state.pendingCompositionFrames;
        state.lastCommittedFrames = state.pendingFrames;
        state.pendingFrames = 0; state.pendingCompositionFrames = 0;
        for (auto& followed : state.followed) followed.committed = followed.pending;
    }
    callbackActive_ = false;
    return true;
}
void PreparedLiveScorePlayback::rollback() noexcept
{
    timelineDecisionPending_ = false; preparingTimelineFrame_ = false;
    for (auto& state : states_)
    {
        state.callback->rollback(); state.pendingFrames = 0; state.pendingCompositionFrames = 0;
        for (auto& followed : state.followed) followed.pending = followed.committed;
    }
    callbackActive_ = false;
}
// A source restored to a prepared point holds the values it started with,
// and follows the parent's again where they were changed live.
void PreparedLiveScorePlayback::restartFollowedValues() noexcept
{
    for (auto& state : states_)
        for (auto& followed : state.followed)
            followed.committed = followed.pending = std::numeric_limits<double>::quiet_NaN();
}
void PreparedLiveScorePlayback::collectOfflineAnalysis(RealtimeRenderSession::OfflineTransaction& destination)
{
    if (offline_) for (std::size_t s = 0; s < states_.size(); ++s)
        if (states_[s].lastCommittedFrames) offline_->collect(s, states_[s].lastCommittedFrames, destination);
}
const double* PreparedLiveScorePlayback::channel(std::size_t clip, std::size_t channel) const noexcept
{
    return clip < audio_.size() && channel < audio_[clip].size() ? audio_[clip][channel].data() : nullptr;
}
std::unique_ptr<PreparedLiveScorePlayback::OfflineCheckpoint> PreparedLiveScorePlayback::checkpoint()
{ return std::make_unique<OfflineCheckpoint>(*this); }
PreparedLiveScorePlayback::OfflineCheckpoint::OfflineCheckpoint(PreparedLiveScorePlayback& owner) : owner_(owner)
{
    if (owner_.offline_) throw std::logic_error("Nested live Score offline checkpoint");
    if (owner_.callbackActive_) throw std::logic_error("Live Score offline checkpoint during an active callback");
    positioned_ = owner_.positioned_;
    for (auto& source : owner_.states_)
    {
        positions_.push_back(source.position);
        for (const auto& followed : source.followed) followed_.push_back(followed.committed);
        auto child = source.callback->beginOfflineTransaction();
        if (!child) throw std::logic_error("Live Score child offline checkpoint unavailable");
        children_.push_back(std::move(child));
    }
    owner_.offline_ = this;
}
PreparedLiveScorePlayback::OfflineCheckpoint::~OfflineCheckpoint()
{
    if (!committed_)
    {
        // Retire callback journals before the child offline journals restore
        // their owned storage and decoded stream views.
        owner_.rollback();
        for (std::size_t s = 0; s < positions_.size(); ++s) owner_.states_[s].position = positions_[s];
        std::size_t followed = 0;
        for (auto& source : owner_.states_)
            for (auto& value : source.followed)
                if (followed < followed_.size()) value.committed = value.pending = followed_[followed++];
        owner_.positioned_ = positioned_;
    }
    owner_.offline_ = nullptr;
}
bool PreparedLiveScorePlayback::OfflineCheckpoint::commit() noexcept
{
    if (owner_.callbackActive_) return false;
    for (auto& child : children_) if (!child->commit()) return false;
    committed_ = true; return true;
}
void PreparedLiveScorePlayback::OfflineCheckpoint::collect(std::size_t source, std::size_t frames,
    RealtimeRenderSession::OfflineTransaction& destination)
{ destination.collectAnalysisFrom(*children_[source], frames); }
}
