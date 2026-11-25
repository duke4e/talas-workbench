#pragma once

#include "talas/project/ProjectMidiVoiceIslands.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace talas::runtime::prepared_voice_hierarchy
{
inline constexpr std::size_t noIndex =
    std::numeric_limits<std::size_t>::max();

struct Layout
{
    bool hierarchical = false;
    std::vector<std::size_t> firstVoiceStateByRegion;
    std::vector<std::size_t> parentVoiceStateByRegion;
    std::vector<std::size_t> voiceStateByStep;
    std::vector<std::size_t> regionByOwnerStep;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Validate the tree encoded by MIDI owner steps, voice scopes, and nested
// half-open step ranges. State indices follow the same region/voice order used
// by realtime preparation.
[[nodiscard]] inline Layout prepare(const RuntimePlan& plan)
{
    Layout layout;
    const auto reject = [&layout](std::string message) -> Layout {
        layout.error = std::move(message);
        return std::move(layout);
    };

    constexpr auto maximumVoiceStates =
        talas::project::maximumPreparedMidiVoiceHierarchyScopes;
    constexpr auto maximumNestingDepth =
        talas::project::maximumPreparedMidiVoiceNestingDepth;
    constexpr auto maximumVoicesPerRegion =
        talas::project::maximumPreparedMidiVoiceSlotsPerOwner;
    if (plan.midiVoices.size() > maximumVoiceStates)
        return reject("prepared MIDI Voice region count exceeds its limit");

    std::size_t totalVoiceStates = 0;
    for (const auto& region : plan.midiVoices)
    {
        if (region.voices.empty()
            || region.voices.size() != region.configuration.polyphony
            || region.voices.size() > maximumVoicesPerRegion)
            return reject("prepared MIDI Voice region has an invalid local voice count");
        if (region.voices.size() > maximumVoiceStates - totalVoiceStates)
            return reject("prepared MIDI Voice state count exceeds its limit");
        totalVoiceStates += region.voices.size();
    }

    layout.firstVoiceStateByRegion.resize(plan.midiVoices.size(), noIndex);
    layout.parentVoiceStateByRegion.resize(plan.midiVoices.size(), noIndex);
    layout.voiceStateByStep.resize(plan.steps.size(), noIndex);
    layout.regionByOwnerStep.resize(plan.steps.size(), noIndex);

    struct VoiceInterval
    {
        std::size_t stateIndex = noIndex;
        std::size_t regionIndex = noIndex;
        std::size_t first = 0;
        std::size_t end = 0;
    };
    std::vector<VoiceInterval> intervals;
    intervals.reserve(totalVoiceStates);
    std::vector<std::size_t> regionByVoiceState;
    std::vector<std::size_t> ordinalByVoiceState;
    regionByVoiceState.reserve(totalVoiceStates);
    ordinalByVoiceState.reserve(totalVoiceStates);
    std::vector<std::pair<RuntimeScopeId, std::size_t>> stateByScope;
    stateByScope.reserve(totalVoiceStates);

    std::size_t nextVoiceState = 0;
    for (std::size_t regionIndex = 0;
         regionIndex < plan.midiVoices.size();
         ++regionIndex)
    {
        const auto& region = plan.midiVoices[regionIndex];
        layout.firstVoiceStateByRegion[regionIndex] = nextVoiceState;
        if (region.ownerStepIndex >= plan.steps.size())
            return reject("prepared MIDI Voice owner step is outside the plan");
        if (layout.regionByOwnerStep[region.ownerStepIndex] != noIndex)
            return reject("prepared MIDI Voice owners share a step");
        layout.regionByOwnerStep[region.ownerStepIndex] = regionIndex;

        std::size_t previousVoiceRangeEnd = 0;
        for (std::size_t ordinal = 0;
             ordinal < region.voices.size();
             ++ordinal)
        {
            const auto& voice = region.voices[ordinal];
            if (voice.voiceOrdinal != ordinal)
                return reject("prepared MIDI Voice ordinals are not local and contiguous");
            if (voice.scopeId == globalRuntimeScope)
                return reject("prepared MIDI Voice scope cannot be global");
            if (voice.firstStepIndex >= plan.steps.size()
                || voice.stepCount == 0
                || voice.stepCount > plan.steps.size() - voice.firstStepIndex)
                return reject("prepared MIDI Voice range is empty or outside the plan");
            const auto voiceRangeEnd =
                voice.firstStepIndex + voice.stepCount;
            if (voiceRangeEnd > region.ownerStepIndex)
                return reject("prepared MIDI Voice range extends past its owner step");
            if (ordinal > 0
                && voice.firstStepIndex < previousVoiceRangeEnd)
                return reject("prepared MIDI Voice ranges are not disjoint in ordinal order");
            previousVoiceRangeEnd = voiceRangeEnd;

            const auto stateIndex = nextVoiceState++;
            intervals.push_back({
                stateIndex,
                regionIndex,
                voice.firstStepIndex,
                voiceRangeEnd,
            });
            regionByVoiceState.push_back(regionIndex);
            ordinalByVoiceState.push_back(ordinal);
            stateByScope.emplace_back(voice.scopeId, stateIndex);
        }
    }

    std::ranges::sort(stateByScope);
    if (std::adjacent_find(
            stateByScope.begin(), stateByScope.end(),
            [](const auto& left, const auto& right) {
                return left.first == right.first;
            }) != stateByScope.end())
        return reject("prepared MIDI Voice scopes are duplicated");

    const auto stateForScope = [&](RuntimeScopeId scopeId)
        -> std::optional<std::size_t> {
        const auto found = std::lower_bound(
            stateByScope.begin(), stateByScope.end(), scopeId,
            [](const auto& entry, RuntimeScopeId candidate) {
                return entry.first < candidate;
            });
        if (found == stateByScope.end() || found->first != scopeId)
            return std::nullopt;
        return found->second;
    };

    for (std::size_t regionIndex = 0;
         regionIndex < plan.midiVoices.size();
         ++regionIndex)
    {
        const auto& region = plan.midiVoices[regionIndex];
        if (!region.parentVoiceScopeId.has_value())
        {
            if (!region.voiceAncestors.empty())
                return reject("root MIDI Voice region has parent ancestry");
            continue;
        }

        const auto parentState = stateForScope(*region.parentVoiceScopeId);
        if (!parentState.has_value())
            return reject("nested MIDI Voice parent scope does not resolve to one voice");
        layout.parentVoiceStateByRegion[regionIndex] = *parentState;

        const auto parentRegionIndex = regionByVoiceState[*parentState];
        const auto parentOrdinal = ordinalByVoiceState[*parentState];
        const auto& parentRegion = plan.midiVoices[parentRegionIndex];
        const auto& parentVoice = parentRegion.voices[parentOrdinal];
        const auto& ownerStep = plan.steps[region.ownerStepIndex];
        if (ownerStep.scopeId != parentVoice.scopeId
            || region.ownerStepIndex < parentVoice.firstStepIndex
            || region.ownerStepIndex - parentVoice.firstStepIndex
                >= parentVoice.stepCount)
            return reject("nested MIDI Voice owner step is outside its parent voice");

        if (region.voiceAncestors.size()
                != parentRegion.voiceAncestors.size() + 1
            || !std::equal(
                parentRegion.voiceAncestors.begin(),
                parentRegion.voiceAncestors.end(),
                region.voiceAncestors.begin()))
            return reject("nested MIDI Voice ancestry does not match its parent");
        const talas::project::ProjectMidiVoiceAncestorIdentity expectedParent{
            parentRegion.ownerNodeId,
            parentRegion.ownerDefinitionId,
            parentRegion.ownerInstancePath,
            parentVoice.voiceOrdinal,
        };
        if (region.voiceAncestors.back() != expectedParent)
            return reject("nested MIDI Voice ancestry identity is inconsistent");
        layout.hierarchical = true;
    }

    for (std::size_t regionIndex = 0;
         regionIndex < plan.midiVoices.size();
         ++regionIndex)
    {
        auto ancestorRegion = regionIndex;
        std::size_t depth = 1;
        while (layout.parentVoiceStateByRegion[ancestorRegion] != noIndex)
        {
            if (depth >= maximumNestingDepth)
                return reject("prepared MIDI Voice nesting exceeds its depth limit");
            ancestorRegion = regionByVoiceState[
                layout.parentVoiceStateByRegion[ancestorRegion]];
            ++depth;
        }
    }

    std::ranges::sort(intervals, [](const auto& left, const auto& right) {
        if (left.first != right.first) return left.first < right.first;
        if (left.end != right.end) return left.end > right.end;
        return left.stateIndex < right.stateIndex;
    });
    std::vector<std::size_t> activeIntervals;
    activeIntervals.reserve(maximumNestingDepth);
    for (const auto& interval : intervals)
    {
        while (!activeIntervals.empty()
            && interval.first >= intervals[activeIntervals.back()].end)
            activeIntervals.pop_back();

        const auto parentState =
            layout.parentVoiceStateByRegion[interval.regionIndex];
        if (activeIntervals.empty())
        {
            if (parentState != noIndex)
                return reject("nested MIDI Voice range has no enclosing parent range");
        }
        else
        {
            const auto& enclosing = intervals[activeIntervals.back()];
            if (interval.end > enclosing.end
                || (interval.first == enclosing.first
                    && interval.end == enclosing.end))
                return reject("prepared MIDI Voice ranges overlap without strict containment");
            if (parentState != enclosing.stateIndex)
                return reject("overlapping MIDI Voice ranges are not parent and child");
        }
        activeIntervals.push_back(
            static_cast<std::size_t>(&interval - intervals.data()));
    }

    std::vector<std::size_t> active;
    active.reserve(maximumNestingDepth);
    std::size_t nextInterval = 0;
    for (std::size_t stepIndex = 0;
         stepIndex < plan.steps.size();
         ++stepIndex)
    {
        while (!active.empty()
            && intervals[active.back()].end <= stepIndex)
            active.pop_back();
        while (nextInterval < intervals.size()
            && intervals[nextInterval].first == stepIndex)
        {
            active.push_back(nextInterval++);
        }

        const auto knownState = stateForScope(plan.steps[stepIndex].scopeId);
        if (active.empty())
        {
            if (knownState.has_value())
                return reject("prepared MIDI Voice scope has a step outside its range");
        }
        else
        {
            if (!knownState.has_value()
                || *knownState != intervals[active.back()].stateIndex)
                return reject("enclosed step has no unique direct MIDI Voice owner");
            layout.voiceStateByStep[stepIndex] = *knownState;
        }

        const auto regionIndex = layout.regionByOwnerStep[stepIndex];
        if (regionIndex == noIndex) continue;
        const auto parentState =
            layout.parentVoiceStateByRegion[regionIndex];
        if (parentState == noIndex)
        {
            if (!active.empty())
                return reject("root MIDI Voice owner step is inside a voice range");
        }
        else if (active.empty()
            || intervals[active.back()].stateIndex != parentState
            || !knownState.has_value()
            || *knownState != parentState)
            return reject("nested MIDI Voice owner step is not directly owned by its parent voice");
    }

    return layout;
}
}
