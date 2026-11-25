#include "talas/runtime/RuntimeConformance.h"
#include "talas/runtime/RealtimeRenderSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>

namespace talas::runtime
{
namespace
{
bool validPartition(std::span<const std::size_t> partition,
                    std::size_t frames, std::size_t maximum)
{
    if (partition.empty()) return false;
    std::size_t total = 0;
    for (const auto block : partition)
    {
        if (block > maximum || block > frames - total) return false;
        total += block;
    }
    return total == frames;
}

bool samplesDiffer(double a, double b, ConformanceTolerance tolerance)
{
    if (!std::isfinite(a) || !std::isfinite(b)) return true;
    const auto scale = std::max(std::abs(a), std::abs(b));
    const auto allowed = tolerance.absolute + tolerance.relative * scale;
    const auto difference = std::abs(a - b);
    // Avoid an infinity > infinity false pass for finite extreme values.
    return std::isfinite(difference) && std::isfinite(allowed)
        ? difference > allowed
        : scale > 0.0 && std::abs(a / scale - b / scale)
            > tolerance.absolute / scale + tolerance.relative;
}

bool eventMismatch(RuntimeConformanceResult& result, std::string_view comparison,
                   std::string_view stream, std::size_t index, std::string_view field)
{
    result.eventMismatch = ConformanceEventMismatch{
        std::string(comparison), std::string(stream), index, std::string(field)};
    result.message = std::string(comparison) + " " + std::string(stream)
        + " event " + std::to_string(index) + " differs: " + std::string(field);
    return false;
}

template <class Event>
bool compareEvents(std::span<const Event> expected, std::span<const Event> actual,
                   ConformanceTolerance tolerance, std::string_view comparison,
                   std::string_view stream, RuntimeConformanceResult& result)
{
    if (expected.size() != actual.size())
        return eventMismatch(result, comparison, stream,
                             std::min(expected.size(), actual.size()), "count");
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        const auto& a = expected[index];
        const auto& b = actual[index];
        if (a.frame != b.frame)
            return eventMismatch(result, comparison, stream, index, "frame/order");
        if constexpr (std::is_same_v<Event, MidiEvent>)
        {
            if (a.bytes != b.bytes)
                return eventMismatch(result, comparison, stream, index, "bytes/order");
        }
        else
        {
            if (a.name != b.name)
                return eventMismatch(result, comparison, stream, index, "name/order");
            if (samplesDiffer(a.value, b.value, tolerance))
                return eventMismatch(result, comparison, stream, index, "value");
            if constexpr (std::is_same_v<Event, GlobalControllerEvent>)
                if (a.allowLiveOverride != b.allowLiveOverride)
                    return eventMismatch(result, comparison, stream, index, "allowLiveOverride");
        }
    }
    return true;
}

template <class Event>
bool validEvents(std::span<const Event> events, std::size_t frames)
{
    std::size_t previous = 0;
    for (const auto& event : events)
    {
        if (event.frame >= frames || event.frame < previous) return false;
        if constexpr (std::is_same_v<Event, MidiEvent>)
        {
            if (event.bytes.empty()) return false;
        }
        else if (event.name.empty() || !std::isfinite(event.value)) return false;
        previous = event.frame;
    }
    return true;
}

bool compareChannels(std::span<const std::vector<double>> expected,
                     std::span<const std::vector<double>> actual,
                     ConformanceTolerance tolerance,
                     std::string_view comparison,
                     RuntimeConformanceResult& result)
{
    if (expected.size() != actual.size())
    {
        result.message = std::string(comparison) + " channel counts differ";
        return false;
    }
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        if (expected[channel].size() != actual[channel].size())
        {
            result.message = std::string(comparison) + " channel sizes differ";
            return false;
        }
        for (std::size_t frame = 0; frame < expected[channel].size(); ++frame)
        {
            const auto a = expected[channel][frame];
            const auto b = actual[channel][frame];
            const auto scale = std::max(std::abs(a), std::abs(b));
            const auto allowed = tolerance.absolute + tolerance.relative * scale;
            if (samplesDiffer(a, b, tolerance))
            {
                result.mismatch = ConformanceMismatch{
                    channel, frame, a, b, allowed, std::string(comparison)};
                result.message = std::string(comparison) + " mismatch at channel "
                    + std::to_string(channel) + ", frame " + std::to_string(frame);
                return false;
            }
        }
    }
    return true;
}
}

RuntimeConformanceResult compareOfflineAndPreparedRealtime(
    const PreparedRuntimeGraph& graph, std::size_t frameCount,
    std::span<const std::size_t> realtimeBlockPartition,
    ConformanceTolerance tolerance)
{
    return runRuntimeConformance(graph, frameCount, realtimeBlockPartition, {}, tolerance);
}

namespace
{
bool probeTerminalCapacity(RealtimeRenderSession& realtime, std::size_t frames,
    const ConformanceTerminalCapacityProbe& probe,
    std::vector<RealtimeMidiOutputEvent>& midiStorage,
    std::vector<RealtimeGlobalControllerOutputEvent>& controllerStorage,
    std::vector<char>& nameStorage, RuntimeConformanceResult& result)
{
    const auto midiCapacity = probe.midiEvents.value_or(midiStorage.size());
    const auto controllerCapacity = probe.controllerEvents.value_or(controllerStorage.size());
    const auto nameCapacity = probe.controllerNameBytes.value_or(nameStorage.size());
    if (midiCapacity > midiStorage.size() || controllerCapacity > controllerStorage.size()
        || nameCapacity > nameStorage.size())
    {
        result.message = "terminal capacity probe exceeds the normal caller storage";
        return false;
    }
    // Guard the complete backing stores, including space outside the supplied
    // prefixes. Compare fields rather than object padding.
    for (auto& event : midiStorage)
    {
        event.frameOffset = 91;
        event.byteCount = 92;
        event.bytes.fill(0xa5);
    }
    for (auto& event : controllerStorage) event = {93, -123.25};
    std::ranges::fill(nameStorage, 'Q');
    RealtimeTerminalOutputBlock output{};
    output.midiEvents = std::span{midiStorage}.first(midiCapacity);
    output.globalControllerEvents = std::span{controllerStorage}.first(controllerCapacity);
    output.globalControllerName = std::span{nameStorage}.first(nameCapacity);
    output.midiEventCount = 94;
    output.globalControllerEventCount = 95;
    output.globalControllerNameLength = 96;
    output.stageId = 97;
    const auto before = output;
    const auto outcome = realtime.renderTerminalEvents(frames, output);
    if (outcome != RealtimeTerminalRenderResult::CapacityExceeded)
    {
        result.message = "terminal capacity probe did not return CapacityExceeded at block "
            + std::to_string(probe.blockIndex);
        return false;
    }
    const auto unchanged = output.midiEventCount == before.midiEventCount
        && output.globalControllerEventCount == before.globalControllerEventCount
        && output.globalControllerNameLength == before.globalControllerNameLength
        && output.stageId == before.stageId
        && output.midiEvents.data() == before.midiEvents.data()
        && output.midiEvents.size() == before.midiEvents.size()
        && output.globalControllerEvents.data() == before.globalControllerEvents.data()
        && output.globalControllerEvents.size() == before.globalControllerEvents.size()
        && output.globalControllerName.data() == before.globalControllerName.data()
        && output.globalControllerName.size() == before.globalControllerName.size()
        && std::ranges::all_of(midiStorage, [](const auto& event) {
            return event.frameOffset == 91 && event.byteCount == 92
                && std::ranges::all_of(event.bytes, [](auto byte) { return byte == 0xa5; }); })
        && std::ranges::all_of(controllerStorage, [](const auto& event) {
            return event.frameOffset == 93 && event.value == -123.25; })
        && std::ranges::all_of(nameStorage, [](auto byte) { return byte == 'Q'; });
    if (!unchanged)
    {
        result.message = "failed terminal capacity probe modified caller storage or metadata at block "
            + std::to_string(probe.blockIndex);
        return false;
    }
    return true;
}

bool prepareConformanceInputs(const PreparedRuntimeGraph& graph,
    const RealtimeRenderSession& realtime, std::size_t frameCount,
    const ConformanceRenderOptions& options, std::vector<OfflineMidiVoiceEvent>& offlineEvents,
    std::vector<PreparedTypedEvent>& preparedInputs, bool& usesCombinedBackend,
    RuntimeConformanceResult& result)
{
    const bool explicitOrder = !options.inputEvents.empty();
    if (explicitOrder && (!options.midiNotes.empty() || !options.variableAssignments.empty()))
    {
        result.message = "explicit input events cannot be combined with convenience schedules";
        return false;
    }
    if (!std::ranges::is_sorted(options.midiNotes, {}, &ConformanceMidiNote::frame)
        || !std::ranges::is_sorted(options.variableAssignments, {}, &ConformanceVariableAssignment::frame))
    {
        result.message = "conformance convenience schedules must be ordered by frame";
        return false;
    }
    std::vector<ConformanceInputEvent> convenience;
    std::size_t noteCursor = 0;
    std::size_t variableCursor = 0;
    while (noteCursor < options.midiNotes.size() || variableCursor < options.variableAssignments.size())
    {
        const bool variable = variableCursor < options.variableAssignments.size()
            && (noteCursor == options.midiNotes.size()
                || options.variableAssignments[variableCursor].frame <= options.midiNotes[noteCursor].frame);
        if (variable) convenience.emplace_back(options.variableAssignments[variableCursor++]);
        else convenience.emplace_back(options.midiNotes[noteCursor++]);
    }
    const auto inputs = explicitOrder ? options.inputEvents : std::span<const ConformanceInputEvent>{convenience};
    offlineEvents.reserve(inputs.size());
    preparedInputs.reserve(inputs.size());
    std::size_t previousFrame = 0;
    for (const auto& input : inputs)
    {
        if (input.valueless_by_exception())
        {
            result.message = "conformance input event has no payload";
            return false;
        }
        const auto frame = std::visit([](const auto& event) { return event.frame; }, input);
        if (frame >= frameCount || frame < previousFrame)
        {
            result.message = "conformance input events must be ordered and inside the render horizon";
            return false;
        }
        OfflineMidiVoiceEvent event;
        PreparedTypedEvent prepared{};
        if (const auto* note = std::get_if<ConformanceMidiNote>(&input))
        {
            if ((note->kind != ConformanceMidiNoteKind::NoteOn && note->kind != ConformanceMidiNoteKind::NoteOff)
                || note->channel < 1 || note->channel > 16 || note->note > 127 || note->velocity > 127)
            {
                result.message = "MIDI conformance input must contain a valid note payload";
                return false;
            }
            event.kind = note->kind == ConformanceMidiNoteKind::NoteOn
                ? OfflineMidiVoiceEventKind::NoteOn : OfflineMidiVoiceEventKind::NoteOff;
            event.channel = note->channel;
            event.note = note->note;
            event.velocity = note->velocity;
            prepared.kind = note->kind == ConformanceMidiNoteKind::NoteOn
                ? TypedEventKind::MidiNoteOn : TypedEventKind::MidiNoteOff;
            prepared.target = {TypedEventTargetKind::MidiVoice, 1, 1, 1};
            prepared.payload.midiNote = {note->channel, note->note, note->velocity};
            usesCombinedBackend = true;
        }
        else if (const auto* assignment = std::get_if<ConformanceEventValueAssignment>(&input))
        {
            const auto target = realtime.eventValueTargetIndex(assignment->scopeId,
                assignment->name, assignment->domain);
            if (!target || !std::isfinite(assignment->value))
            {
                result.message = "Event Value conformance input requires exact writable scope/name/domain and finite value";
                return false;
            }
            event.kind = OfflineMidiVoiceEventKind::EventValueAssignment;
            event.eventValueName = assignment->name;
            event.eventValueScopeId = assignment->scopeId;
            event.value = assignment->value;
            event.domain = assignment->domain;
            prepared.kind = TypedEventKind::EventValueAssignment;
            prepared.target = {TypedEventTargetKind::EventValue, *target, 1, 1};
            prepared.payload.eventValue = {assignment->value, assignment->domain};
            usesCombinedBackend = true;
        }
        else if (std::holds_alternative<ConformanceSoundVariableAssignment>(input))
        {
            const auto& assignment = std::get<ConformanceSoundVariableAssignment>(input);
            const bool voiceOwned = assignment.target.ownerKind == talas::project::ProjectVariableOwnerKind::MidiVoice;
            std::optional<PreparedSoundVariableTargetIndex> target;
            if (voiceOwned && assignment.voiceGeneration != 0)
            {
                const auto& table = graph.preparedSoundVariables();
                const auto cell = talas::project::findPreparedProjectSoundVariableCell(table, assignment.target);
                if (cell && *cell < table.cells.size() && *cell < std::numeric_limits<std::uint32_t>::max())
                {
                    const auto& candidates = table.cells[*cell].candidateDefinitionIds;
                    const auto candidate = std::ranges::find(candidates, assignment.selectedDefinitionId);
                    if (candidate != candidates.end()
                        && std::distance(candidates.begin(), candidate) <= std::numeric_limits<std::uint16_t>::max())
                        target = PreparedSoundVariableTargetIndex{static_cast<std::uint32_t>(*cell + 1U),
                            static_cast<std::uint16_t>(std::distance(candidates.begin(), candidate))};
                }
            }
            else if (!voiceOwned && assignment.voiceGeneration == 0)
                target = realtime.soundVariableTargetIndex(assignment.target, assignment.selectedDefinitionId);
            if (!target)
            {
                result.message = "Sound variable conformance input requires exact identity, candidate and owner generation";
                return false;
            }
            event.kind = OfflineMidiVoiceEventKind::SoundVariableAssignment;
            event.soundTarget = assignment.target;
            event.selectedDefinitionId = assignment.selectedDefinitionId;
            event.voiceGeneration = assignment.voiceGeneration;
            prepared.kind = TypedEventKind::SoundVariableAssignment;
            prepared.target = {TypedEventTargetKind::SoundVariable, target->index, 1, 1, assignment.voiceGeneration};
            prepared.payload.soundVariable = {target->selectedOrdinal};
            usesCombinedBackend = usesCombinedBackend || voiceOwned;
        }
        else
        {
            const auto& assignment = std::get<ConformanceVariableAssignment>(input);
            const bool voiceOwned = assignment.target.ownerKind == talas::project::ProjectVariableOwnerKind::MidiVoice;
            std::optional<std::uint32_t> target;
            if (voiceOwned && explicitOrder && assignment.voiceGeneration != 0)
            {
                // Resolve stable preparation identity, not current publication:
                // a preceding note in this transaction may create the slot.
                // Execution validates the requested generation atomically.
                const auto& cells = graph.preparedVariables().cells;
                const auto cell = talas::project::findPreparedProjectVariableCell(
                    graph.preparedVariables(), assignment.target);
                if (cell && *cell < cells.size() && *cell < std::numeric_limits<std::uint32_t>::max()
                    && cells[*cell].domain == assignment.domain)
                    target = static_cast<std::uint32_t>(*cell + 1U);
            }
            else if (!voiceOwned && assignment.voiceGeneration == 0)
                target = realtime.variableTargetIndex(assignment.target, assignment.domain);
            if (!target || !std::isfinite(assignment.value))
            {
                result.message = "variable conformance input requires exact identity, domain and owner generation";
                return false;
            }
            event.kind = OfflineMidiVoiceEventKind::VariableAssignment;
            event.target = assignment.target;
            event.value = assignment.value;
            event.domain = assignment.domain;
            event.voiceGeneration = assignment.voiceGeneration;
            prepared.kind = TypedEventKind::VariableAssignment;
            prepared.target = {TypedEventTargetKind::Variable, *target, 1, 1, assignment.voiceGeneration};
            prepared.payload.variable = {assignment.value, assignment.domain};
            usesCombinedBackend = usesCombinedBackend || voiceOwned;
        }
        event.frame = frame;
        event.batchId = frame + 1;
        event.sequence = offlineEvents.size() + 1;
        prepared.frameOffset = frame;
        prepared.sequence = event.sequence;
        offlineEvents.push_back(std::move(event));
        preparedInputs.push_back(prepared);
        previousFrame = frame;
    }
    return true;
}

bool isPartitionBoundary(std::span<const std::size_t> partition, std::size_t frame)
{
    std::size_t total = 0;
    if (frame == 0) return true;
    for (const auto block : partition)
    {
        total += block;
        if (total == frame) return true;
    }
    return false;
}

bool observeSoundState(std::size_t frame, bool afterReset,
    const ConformanceRenderOptions& options, OfflineRenderSession* offline,
    RealtimeRenderSession& realtime, std::size_t cellCount, RuntimeConformanceResult& result)
{
    const auto checkpoint = std::ranges::find(options.soundStateCheckpoints, frame,
        &ConformanceSoundStateCheckpoint::frame);
    if (checkpoint == options.soundStateCheckpoints.end()) return true;
    ConformanceSoundStateObservation observation{frame, afterReset};
    if (offline)
    {
        observation.values = offline->soundVariableValues();
        observation.publicationValid = observation.values.size() == cellCount;
    }
    else
    {
        const auto snapshot = realtime.preparedSoundVariableValues();
        observation.publicationValid = snapshot.publicationValid && snapshot.values.size() == cellCount;
        for (const auto& value : snapshot.values)
            observation.values.push_back({value.identity, value.selectedDefinitionId});
    }
    auto& observations = offline ? result.offlineSoundStates : result.realtimeSoundStates;
    observations.push_back(std::move(observation));
    const auto& actual = observations.back();
    const auto& expected = afterReset && checkpoint->expectedAfterReset
        ? *checkpoint->expectedAfterReset : checkpoint->expected;
    const auto fail = [&](std::size_t index, std::string field) {
        result.stateMismatch = ConformanceStateMismatch{
            offline ? "reference/offline" : "reference/realtime", frame, index, std::move(field), afterReset};
        result.message = "Sound state oracle differs at frame " + std::to_string(frame)
            + " in " + result.stateMismatch->field;
        return false;
    };
    if (!actual.publicationValid) return fail(0, "publicationValid");
    if (actual.values.size() != expected.size()) return fail(0, "count");
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        if (!(actual.values[index].identity == expected[index].identity)) return fail(index, "identity");
        if (actual.values[index].selectedDefinitionId != expected[index].selectedDefinitionId)
            return fail(index, "selectedDefinitionId");
    }
    return true;
}

RuntimeConformanceResult runConformancePass(
    const PreparedRuntimeGraph& graph, OfflineRenderSession& offline,
    RealtimeRenderSession& realtime, std::size_t frameCount,
    std::span<const std::size_t> realtimeBlockPartition,
    const ConformanceRenderOptions& options, ConformanceTolerance tolerance, bool afterReset)
{
    RuntimeConformanceResult result;
    if (!validPartition(realtimeBlockPartition, frameCount, realtime.maxBlockFrames()))
    {
        result.message = "realtime partition must cover the requested frames within the prepared maximum block size";
        return result;
    }
    const std::array defaultOfflinePartition{frameCount};
    const auto offlinePartition = options.offlineBlockPartition.empty()
        ? std::span<const std::size_t>{defaultOfflinePartition} : options.offlineBlockPartition;
    if (!validPartition(offlinePartition, frameCount, frameCount))
    {
        result.message = "offline partition must cover the requested frames exactly";
        return result;
    }
    std::optional<std::size_t> previousCheckpoint;
    for (const auto& checkpoint : options.soundStateCheckpoints)
    {
        if (checkpoint.frame > frameCount || (previousCheckpoint && checkpoint.frame <= *previousCheckpoint)
            || !isPartitionBoundary(offlinePartition, checkpoint.frame)
            || !isPartitionBoundary(realtimeBlockPartition, checkpoint.frame)
            || (checkpoint.expectedAfterReset && !options.verifyResetReplay))
        {
            result.message = "Sound state checkpoints require strictly ordered shared partition boundaries and enabled reset expectations";
            return result;
        }
        previousCheckpoint = checkpoint.frame;
    }
    const auto cellCount = graph.preparedSoundVariables().cells.size();
    if (!observeSoundState(0, afterReset, options, &offline, realtime, cellCount, result)
        || !observeSoundState(0, afterReset, options, nullptr, realtime, cellCount, result)) return result;
    const auto channels = realtime.outputChannelCount();
    std::size_t previousProbeBlock = 0;
    for (const auto& probe : options.terminalCapacityProbes)
    {
        if (channels != 0 || probe.blockIndex >= realtimeBlockPartition.size()
            || realtimeBlockPartition[probe.blockIndex] == 0 || probe.blockIndex < previousProbeBlock
            || (!probe.midiEvents && !probe.controllerEvents && !probe.controllerNameBytes))
        {
            result.message = "terminal capacity probes must name ordered, positive event-only callbacks and an explicit capacity";
            return result;
        }
        previousProbeBlock = probe.blockIndex;
    }
    if ((options.referenceMidiEvents
         && !validEvents(*options.referenceMidiEvents, frameCount))
        || (options.referenceGlobalControllerEvents
            && !validEvents(*options.referenceGlobalControllerEvents, frameCount)))
    {
        result.message = "terminal reference events must be ordered, in range and have valid payloads";
        return result;
    }
    if (!options.referenceChannels.empty())
    {
        if (options.referenceChannels.size() != channels
            || std::ranges::any_of(options.referenceChannels,
                [frameCount](const auto& channel) { return channel.size() != frameCount; }))
        {
            result.message = "reference oracle must contain every channel and requested frame";
            return result;
        }
    }
    std::vector<OfflineMidiVoiceEvent> inputEvents;
    std::vector<PreparedTypedEvent> preparedInputs;
    bool usesCombinedBackend = false;
    const bool useTypedInput = !options.inputEvents.empty() || !options.variableAssignments.empty();
    if (!prepareConformanceInputs(graph, realtime, frameCount, options,
            inputEvents, preparedInputs, usesCombinedBackend, result)) return result;
    const bool hasSoundAssignments = std::ranges::any_of(inputEvents, [](const auto& event) {
        return event.kind == OfflineMidiVoiceEventKind::SoundVariableAssignment;
    });
    const bool hasNumericAssignments = std::ranges::any_of(inputEvents, [](const auto& event) {
        return event.kind == OfflineMidiVoiceEventKind::VariableAssignment;
    });
    if (usesCombinedBackend || (hasSoundAssignments && hasNumericAssignments))
    {
        if (!offline.scheduleMidiVoiceEvents(inputEvents))
        {
            result.message = "offline conformance combined input schedule was rejected";
            return result;
        }
    }
    else if (hasSoundAssignments)
    {
        std::vector<OfflineSoundVariableAssignment> assignments;
        for (const auto& event : inputEvents)
            assignments.push_back({event.frame, event.batchId, event.sequence,
                event.soundTarget, event.selectedDefinitionId});
        if (!offline.scheduleSoundVariableAssignments(assignments))
        {
            result.message = "offline conformance Sound variable schedule was rejected";
            return result;
        }
    }
    else if (!inputEvents.empty())
    {
        std::vector<OfflineVariableAssignment> assignments;
        for (const auto& event : inputEvents)
            assignments.push_back({event.frame, event.batchId, event.sequence,
                event.target, event.value, event.domain});
        if (!offline.scheduleVariableAssignments(assignments))
        {
            result.message = "offline conformance variable schedule was rejected";
            return result;
        }
    }
    result.offlineChannels.resize(channels);
    for (auto& channel : result.offlineChannels) channel.reserve(frameCount);
    std::size_t offlineFrame = 0;
    for (const auto block : offlinePartition)
    {
        const auto before = options.soundStateCheckpoints.empty()
            ? std::vector<OfflineSoundVariableValue>{} : offline.soundVariableValues();
        auto rendered = offline.render(block);
        if (!rendered.ok())
        {
            if (!options.soundStateCheckpoints.empty())
                result.offlineFailureSoundState = ConformanceOfflineFailureSoundState{
                    offlineFrame, block, afterReset, before, offline.soundVariableValues()};
            result.message = "offline conformance render failed";
            return result;
        }
        if (block == 0)
        {
            if (!rendered.mono.empty() || !rendered.right.empty()
                || std::ranges::any_of(rendered.channels, [](const auto& c) { return !c.empty(); })
                || !rendered.midiEvents.empty() || !rendered.globalControllerEvents.empty())
            {
                result.message = "offline zero-frame transaction published output";
                return result;
            }
            continue;
        }
        if (!validEvents<MidiEvent>(rendered.midiEvents, block)
            || !validEvents<GlobalControllerEvent>(rendered.globalControllerEvents, block))
        {
            result.message = "offline terminal transaction returned invalid events";
            return result;
        }
        for (auto& event : rendered.midiEvents)
        {
            event.frame += offlineFrame;
            result.offlineMidiEvents.push_back(std::move(event));
        }
        for (auto& event : rendered.globalControllerEvents)
        {
            event.frame += offlineFrame;
            result.offlineGlobalControllerEvents.push_back(std::move(event));
        }
        offlineFrame += block;
        if (!observeSoundState(offlineFrame, afterReset, options, &offline, realtime, cellCount, result))
            return result;
        // Offline event-only results retain a legacy zero-filled mono view.
        // It is not an audio channel and must not conceal nonzero output.
        if (channels == 0 && (!rendered.right.empty()
            || std::ranges::any_of(rendered.mono, [](double sample) { return sample != 0.0; })))
        {
            result.message = "offline event terminal published unexpected audio";
            return result;
        }
        if (channels != 0 && rendered.channels.empty())
        {
            rendered.channels.push_back(std::move(rendered.mono));
            if (!rendered.right.empty()) rendered.channels.push_back(std::move(rendered.right));
        }
        if (rendered.channels.size() != channels)
        {
            result.message = "offline/prepared channel counts differ";
            return result;
        }
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            if (rendered.channels[channel].size() != block)
            {
                result.message = "offline transaction returned an unexpected frame count";
                return result;
            }
            result.offlineChannels[channel].insert(result.offlineChannels[channel].end(),
                rendered.channels[channel].begin(), rendered.channels[channel].end());
        }
    }
    result.realtimeChannels.assign(channels, std::vector<double>(frameCount));
    std::vector<double*> outputs(channels);
    const auto capacity = channels == 0 ? graph.renderSettings().maxRealtimeTerminalEvents : 0;
    std::vector<RealtimeMidiOutputEvent> midiStorage(capacity);
    std::vector<RealtimeGlobalControllerOutputEvent> controllerStorage(capacity);
    std::size_t nameCapacity = 0;
    for (const auto& event : result.offlineGlobalControllerEvents)
        nameCapacity = std::max(nameCapacity, event.name.size());
    std::vector<char> nameStorage(nameCapacity);
    std::size_t absoluteFrame = 0;
    std::size_t noteIndex = 0;
    std::size_t typedInputIndex = 0;
    std::size_t blockIndex = 0;
    std::size_t probeIndex = 0;
    std::vector<RealtimeTypedEventDispatchResult> inputDispatch(preparedInputs.size());
    const auto loop = realtime.transportLoop();
    for (const auto block : realtimeBlockPartition)
    {
        RealtimeTransportSnapshot snapshot{
            .epoch = 1, .absoluteFrame = absoluteFrame, .playheadFrame = absoluteFrame,
            .sampleRate = realtime.sampleRate(),
            .loopStartFrame = loop ? loop->startFrame : 0,
            .loopEndFrame = loop ? loop->endFrame : 0,
            .discontinuitySequence = 0, .playing = true, .loopEnabled = loop.has_value(),
        };
        snapshot.playheadFrame = transportPlayheadFrame(absoluteFrame, snapshot);
        realtime.setTransportSnapshotBeforeRender(snapshot);
        if (useTypedInput)
        {
            while (typedInputIndex < preparedInputs.size()
                && preparedInputs[typedInputIndex].frameOffset < absoluteFrame + block)
            {
                const auto begin = typedInputIndex;
                const auto eventFrame = preparedInputs[begin].frameOffset;
                while (typedInputIndex < preparedInputs.size()
                    && preparedInputs[typedInputIndex].frameOffset == eventFrame)
                    preparedInputs[typedInputIndex++].frameOffset -= absoluteFrame;
                const auto events = std::span{preparedInputs}.subspan(begin, typedInputIndex - begin);
                const auto dispatches = std::span{inputDispatch}.first(events.size());
                if (!realtime.scheduleTypedEvents(events, dispatches)
                    || !std::ranges::all_of(dispatches, [](auto dispatch) {
                        return dispatch == RealtimeTypedEventDispatchResult::Handled; }))
                {
                    result.message = "prepared realtime conformance input transaction was rejected";
                    return result;
                }
            }
        }
        while (!useTypedInput && noteIndex < options.midiNotes.size()
               && options.midiNotes[noteIndex].frame < absoluteFrame + block)
        {
            const auto& note = options.midiNotes[noteIndex++];
            const auto dispatch = realtime.scheduleMidiNote({
                note.kind == ConformanceMidiNoteKind::NoteOn
                    ? RealtimeMidiNoteKind::NoteOn : RealtimeMidiNoteKind::NoteOff,
                note.channel, note.note, note.velocity}, note.frame - absoluteFrame);
            if (dispatch != RealtimeMidiDispatchResult::Handled)
            {
                result.message = "prepared realtime conformance MIDI schedule was rejected";
                return result;
            }
        }
        for (std::size_t channel = 0; channel < channels; ++channel)
            outputs[channel] = result.realtimeChannels[channel].data() + absoluteFrame;
        const auto firstProbe = probeIndex;
        while (probeIndex < options.terminalCapacityProbes.size()
            && options.terminalCapacityProbes[probeIndex].blockIndex == blockIndex)
        {
            if (!probeTerminalCapacity(realtime, block, options.terminalCapacityProbes[probeIndex],
                    midiStorage, controllerStorage, nameStorage, result)) return result;
            ++probeIndex;
        }
        const auto expectedSuccess = block != 0
            || options.realtimeZeroFrames == ConformanceZeroFramePolicy::RequireSuccess;
        bool rendered = false;
        if (channels == 0)
        {
            RealtimeTerminalOutputBlock output{};
            output.midiEvents = midiStorage;
            output.globalControllerEvents = controllerStorage;
            output.globalControllerName = nameStorage;
            const auto outcome = realtime.renderTerminalEvents(block, output);
            rendered = outcome == RealtimeTerminalRenderResult::Rendered;
            if (block == 0 && !expectedSuccess
                && outcome != RealtimeTerminalRenderResult::InvalidArguments)
            {
                result.message = "empty realtime terminal transaction returned an unexpected rejection";
                return result;
            }
            if (output.midiEventCount > midiStorage.size()
                || output.globalControllerEventCount > controllerStorage.size()
                || output.globalControllerNameLength > nameStorage.size()
                || (!rendered && (output.midiEventCount != 0
                    || output.globalControllerEventCount != 0 || output.globalControllerNameLength != 0)))
            {
                result.message = "realtime terminal transaction published invalid metadata";
                return result;
            }
            std::size_t previous = 0;
            for (std::size_t index = 0; index < output.midiEventCount; ++index)
            {
                const auto& event = midiStorage[index];
                if (event.frameOffset >= block || event.frameOffset < previous
                    || event.byteCount == 0 || event.byteCount > event.bytes.size())
                {
                    result.message = "realtime terminal transaction returned invalid MIDI events";
                    return result;
                }
                result.realtimeMidiEvents.push_back({absoluteFrame + event.frameOffset,
                    {event.bytes.begin(), event.bytes.begin() + event.byteCount}});
                previous = event.frameOffset;
            }
            previous = 0;
            for (std::size_t index = 0; index < output.globalControllerEventCount; ++index)
            {
                const auto& event = controllerStorage[index];
                if (event.frameOffset >= block || event.frameOffset < previous
                    || !std::isfinite(event.value) || output.globalControllerNameLength == 0)
                {
                    result.message = "realtime terminal transaction returned invalid controller events";
                    return result;
                }
                result.realtimeGlobalControllerEvents.push_back({absoluteFrame + event.frameOffset,
                    {nameStorage.data(), output.globalControllerNameLength}, event.value,
                    event.allowLiveOverride});
                previous = event.frameOffset;
            }
        }
        else rendered = realtime.render({}, outputs.data(), channels, block);
        if (rendered != expectedSuccess)
        {
            result.message = "prepared realtime conformance render outcome differs at frame "
                + std::to_string(absoluteFrame) + " for " + std::to_string(block) + " frames";
            return result;
        }
        absoluteFrame += block;
        if (block != 0
            && !observeSoundState(absoluteFrame, afterReset, options, nullptr, realtime, cellCount, result))
            return result;
        result.terminalFailureProbesVerified += probeIndex - firstProbe;
        ++blockIndex;
    }
    if (!compareChannels(result.offlineChannels, result.realtimeChannels,
                         tolerance, "offline/realtime", result)) return result;
    if (!compareEvents<MidiEvent>(result.offlineMidiEvents, result.realtimeMidiEvents,
            tolerance, "offline/realtime", "MIDI", result)
        || !compareEvents<GlobalControllerEvent>(result.offlineGlobalControllerEvents,
            result.realtimeGlobalControllerEvents, tolerance, "offline/realtime", "controller", result))
        return result;
    if (options.referenceMidiEvents
        && (!compareEvents<MidiEvent>(*options.referenceMidiEvents, result.offlineMidiEvents,
                tolerance, "reference/offline", "MIDI", result)
            || !compareEvents<MidiEvent>(*options.referenceMidiEvents, result.realtimeMidiEvents,
                tolerance, "reference/realtime", "MIDI", result))) return result;
    if (options.referenceGlobalControllerEvents
        && (!compareEvents<GlobalControllerEvent>(*options.referenceGlobalControllerEvents,
                result.offlineGlobalControllerEvents, tolerance, "reference/offline", "controller", result)
            || !compareEvents<GlobalControllerEvent>(*options.referenceGlobalControllerEvents,
                result.realtimeGlobalControllerEvents, tolerance, "reference/realtime", "controller", result)))
        return result;
    if (!options.referenceChannels.empty())
    {
        if (!compareChannels(options.referenceChannels, result.offlineChannels,
                             tolerance, "reference/offline", result)) return result;
        (void)compareChannels(options.referenceChannels, result.realtimeChannels,
                              tolerance, "reference/realtime", result);
    }
    return result;
}
}

RuntimeConformanceResult runRuntimeConformance(
    const PreparedRuntimeGraph& graph, std::size_t frameCount,
    std::span<const std::size_t> realtimeBlockPartition,
    const ConformanceRenderOptions& options, ConformanceTolerance tolerance)
{
    RuntimeConformanceResult result;
    if (frameCount == 0)
    {
        result.message = "conformance frame count must be positive";
        return result;
    }
    if (!std::isfinite(tolerance.absolute) || !std::isfinite(tolerance.relative)
        || tolerance.absolute < 0.0 || tolerance.relative < 0.0)
    {
        result.message = "conformance tolerance must be finite and non-negative";
        return result;
    }
    RealtimeRenderSession realtime(graph);
    if (!realtime.ready())
    {
        result.message = "prepared realtime conformance session failed: " + std::string(realtime.error());
        return result;
    }
    OfflineRenderSession offline(graph);
    auto first = runConformancePass(graph, offline, realtime, frameCount,
                                    realtimeBlockPartition, options, tolerance, false);
    if (!first.ok() || !options.verifyResetReplay) return first;

    offline.reset();
    realtime.resetTransport();
    auto replay = runConformancePass(graph, offline, realtime, frameCount,
                                     realtimeBlockPartition, options, tolerance, true);
    if (!replay.ok())
    {
        replay.message = "reset replay: " + replay.message;
        if (replay.mismatch) replay.mismatch->comparison = "reset/" + replay.mismatch->comparison;
        if (replay.eventMismatch) replay.eventMismatch->comparison = "reset/" + replay.eventMismatch->comparison;
        return replay;
    }
    // Compare each path with its own pre-reset trace as well. Internal path
    // agreement alone cannot prove that both sessions actually reset.
    if (!compareChannels(first.offlineChannels, replay.offlineChannels,
            tolerance, "first/reset-offline", replay)
        || !compareChannels(first.realtimeChannels, replay.realtimeChannels,
            tolerance, "first/reset-realtime", replay)
        || !compareEvents<MidiEvent>(first.offlineMidiEvents, replay.offlineMidiEvents,
            tolerance, "first/reset-offline", "MIDI", replay)
        || !compareEvents<MidiEvent>(first.realtimeMidiEvents, replay.realtimeMidiEvents,
            tolerance, "first/reset-realtime", "MIDI", replay)
        || !compareEvents<GlobalControllerEvent>(first.offlineGlobalControllerEvents,
            replay.offlineGlobalControllerEvents, tolerance, "first/reset-offline", "controller", replay)
        || !compareEvents<GlobalControllerEvent>(first.realtimeGlobalControllerEvents,
            replay.realtimeGlobalControllerEvents, tolerance, "first/reset-realtime", "controller", replay))
        return replay;
    first.offlineSoundStates.insert(first.offlineSoundStates.end(),
        replay.offlineSoundStates.begin(), replay.offlineSoundStates.end());
    first.realtimeSoundStates.insert(first.realtimeSoundStates.end(),
        replay.realtimeSoundStates.begin(), replay.realtimeSoundStates.end());
    first.resetReplayVerified = true;
    first.terminalFailureProbesVerified += replay.terminalFailureProbesVerified;
    return first;
}
}
