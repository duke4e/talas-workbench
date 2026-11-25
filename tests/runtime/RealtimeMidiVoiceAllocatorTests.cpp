#include "talas/runtime/RealtimeMidiVoiceAllocator.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace talas::runtime
{
struct RealtimeMidiVoiceAllocatorTestAccess
{
    static void exhaustSlotGeneration(
        RealtimeMidiVoiceAllocator& allocator,
        std::size_t index)
    {
        allocator.slots_[index].generation =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustActivationSequence(
        RealtimeMidiVoiceAllocator& allocator)
    {
        allocator.nextActivationSequence_ =
            std::numeric_limits<std::uint64_t>::max();
    }

    static void exhaustMutationSequence(
        RealtimeMidiVoiceAllocator& allocator)
    {
        allocator.allocatorMutationSequence_ =
            std::numeric_limits<std::uint64_t>::max();
    }
};
}

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
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

namespace
{
using talas::runtime::RealtimeMidiVoiceAction;
using talas::runtime::RealtimeMidiVoiceActionKind;
using talas::runtime::RealtimeMidiVoiceAllocator;
using talas::runtime::RealtimeMidiVoiceConfiguration;

static_assert(noexcept(std::declval<RealtimeMidiVoiceAllocator&>().noteOn(1, 60, 100, 0)));
static_assert(noexcept(std::declval<RealtimeMidiVoiceAllocator&>().noteOff(1, 60, 0)));
static_assert(noexcept(std::declval<const RealtimeMidiVoiceAllocator&>().delayedAttack(0, 1, 0)));
static_assert(noexcept(std::declval<RealtimeMidiVoiceAllocator&>().rollback(
    std::declval<const RealtimeMidiVoiceAction&>())));

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

RealtimeMidiVoiceAction requireAction(
    const std::optional<RealtimeMidiVoiceAction>& action,
    RealtimeMidiVoiceActionKind kind,
    const std::string& message)
{
    require(action.has_value(), message + ": action missing");
    require(action->kind == kind, message + ": action kind");
    return *action;
}

void testAllocatorRejectsInvalidConfiguration()
{
    RealtimeMidiVoiceAllocator defaultAllocator({});
    require(defaultAllocator.ready(), "default allocator should be ready");

    auto invalidChannel = RealtimeMidiVoiceConfiguration{};
    invalidChannel.channel = 0;
    RealtimeMidiVoiceAllocator channelAllocator(invalidChannel);
    require(!channelAllocator.ready(), "channel zero should be invalid");

    invalidChannel.channel = 17;
    RealtimeMidiVoiceAllocator highChannelAllocator(invalidChannel);
    require(!highChannelAllocator.ready(), "channel above sixteen should be invalid");

    auto invalidRange = RealtimeMidiVoiceConfiguration{};
    invalidRange.lowPitch = 100;
    invalidRange.highPitch = 99;
    RealtimeMidiVoiceAllocator rangeAllocator(invalidRange);
    require(!rangeAllocator.ready(), "reversed pitch range should be invalid");

    invalidRange.lowPitch = 0;
    invalidRange.highPitch = 128;
    RealtimeMidiVoiceAllocator highPitchAllocator(invalidRange);
    require(!highPitchAllocator.ready(), "pitch above MIDI range should be invalid");

    auto invalidPolyphony = RealtimeMidiVoiceConfiguration{};
    invalidPolyphony.polyphony = 0;
    RealtimeMidiVoiceAllocator polyphonyAllocator(invalidPolyphony);
    require(!polyphonyAllocator.ready(), "zero polyphony should be invalid");
    require(!polyphonyAllocator.noteOn(1, 60, 100, 0).has_value(), "unready allocator should reject note-ons");

    invalidPolyphony.polyphony = 129;
    RealtimeMidiVoiceAllocator oversizedAllocator(invalidPolyphony);
    require(!oversizedAllocator.ready(), "polyphony above fixed allocator capacity should be invalid");
}

void testAllocatorFiltersChannelAndPitchRange()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.channel = 4;
    configuration.lowPitch = 48;
    configuration.highPitch = 72;
    RealtimeMidiVoiceAllocator allocator(configuration);

    require(allocator.matches(4, 48), "low range boundary should match");
    require(allocator.matches(4, 72), "high range boundary should match");
    require(!allocator.matches(3, 60), "different channel should not match");
    require(!allocator.matches(4, 47), "pitch below range should not match");
    require(!allocator.matches(4, 73), "pitch above range should not match");
    require(!allocator.matches(17, 60), "invalid MIDI channel should not match");
    require(!allocator.matches(4, 128), "invalid MIDI pitch should not match");
    require(!allocator.noteOn(3, 60, 100, 10).has_value(), "filtered note-on should not allocate");
    require(!allocator.noteOn(4, 47, 100, 10).has_value(), "out-of-range note-on should not allocate");
    require(!allocator.noteOn(4, 60, 128, 10).has_value(), "invalid MIDI velocity should not allocate");
}

void testAllocatorSelectsOldestReleasedVoiceAndLowerIndexTies()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.polyphony = 2;
    RealtimeMidiVoiceAllocator allocator(configuration);

    const auto& first = requireAction(allocator.noteOn(1, 60, 90, 10), RealtimeMidiVoiceActionKind::Retrigger,
                                      "first note-on");
    require(first.voiceIndex == 0, "initial released tie should choose lower voice index");
    const auto& second = requireAction(allocator.noteOn(1, 61, 91, 11), RealtimeMidiVoiceActionKind::Retrigger,
                                       "second note-on");
    require(second.voiceIndex == 1, "second initial released tie should choose next lower voice index");
    requireAction(allocator.noteOff(1, 61, 12), RealtimeMidiVoiceActionKind::Release, "release newer voice");
    requireAction(allocator.noteOff(1, 60, 13), RealtimeMidiVoiceActionKind::Release, "release older voice");

    const auto& reused = requireAction(allocator.noteOn(1, 62, 92, 14), RealtimeMidiVoiceActionKind::Retrigger,
                                       "reused note-on");
    require(reused.voiceIndex == 0, "oldest released voice should be reused before a newer release");
}

void testAllocatorStealsLongestHeldVoice()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.polyphony = 2;
    RealtimeMidiVoiceAllocator allocator(configuration);

    requireAction(allocator.noteOn(1, 60, 90, 10), RealtimeMidiVoiceActionKind::Retrigger, "first held voice");
    requireAction(allocator.noteOn(1, 61, 91, 11), RealtimeMidiVoiceActionKind::Retrigger, "second held voice");
    const auto& stolen = requireAction(allocator.noteOn(1, 62, 92, 12), RealtimeMidiVoiceActionKind::Retrigger,
                                       "stealing note-on");
    require(stolen.voiceIndex == 0, "longest-held voice should be stolen");
    require(stolen.note == 62 && stolen.velocity == 92 && stolen.absoluteFrame == 12,
            "steal retrigger should carry new note payload and frame");
}

void testAllocatorStacksRepeatedNotesAndReleasesOldestMatch()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.polyphony = 2;
    RealtimeMidiVoiceAllocator allocator(configuration);

    const auto first = allocator.noteOn(1, 60, 80, 10);
    const auto second = allocator.noteOn(1, 60, 100, 11);
    requireAction(first, RealtimeMidiVoiceActionKind::Retrigger, "first repeated note-on");
    requireAction(second, RealtimeMidiVoiceActionKind::Retrigger, "second repeated note-on");
    require(first->voiceIndex == 0 && second->voiceIndex == 1, "repeated note-ons should occupy independent voices");

    const auto& firstRelease = requireAction(allocator.noteOff(1, 60, 20), RealtimeMidiVoiceActionKind::Release,
                                             "first repeated note-off");
    const auto& secondRelease = requireAction(allocator.noteOff(1, 60, 21), RealtimeMidiVoiceActionKind::Release,
                                              "second repeated note-off");
    require(firstRelease.voiceIndex == 0, "oldest matching note should release first");
    require(secondRelease.voiceIndex == 1, "next matching note should release second");
    require(firstRelease.generation == first->generation
                && secondRelease.generation == second->generation,
            "release should retain each voice generation through its tail");
    require(!allocator.noteOff(1, 60, 22).has_value(), "absent note-off should be ignored");
}

void testAllocatorIgnoresStaleNoteOffAfterSteal()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.polyphony = 1;
    RealtimeMidiVoiceAllocator allocator(configuration);

    const auto first = allocator.noteOn(1, 60, 90, 10);
    const auto second = allocator.noteOn(1, 61, 91, 11);
    requireAction(first, RealtimeMidiVoiceActionKind::Retrigger, "first note-on before steal");
    requireAction(second, RealtimeMidiVoiceActionKind::Retrigger, "second note-on steals first");
    require(!allocator.noteOff(1, 60, 12).has_value(), "stale note-off for stolen assignment should be ignored");
    requireAction(allocator.noteOff(1, 61, 13), RealtimeMidiVoiceActionKind::Release,
                  "current stolen-in assignment should release");
}

void testAllocatorConvertsVelocityZeroToNoteOff()
{
    RealtimeMidiVoiceAllocator allocator({});
    const auto on = allocator.noteOn(1, 60, 100, 10);
    requireAction(on, RealtimeMidiVoiceActionKind::Retrigger, "velocity fixture note-on");

    const auto& release = requireAction(allocator.noteOn(1, 60, 0, 20), RealtimeMidiVoiceActionKind::Release,
                                        "velocity-zero note-on");
    require(release.voiceIndex == on->voiceIndex && release.note == 60 && release.velocity == 0
                && release.absoluteFrame == 20,
            "velocity-zero note-on should preserve note-off payload");
}

void testAllocatorInvalidatesStaleDelayedAttacksByGeneration()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.retriggerDelayFrames = 240;
    RealtimeMidiVoiceAllocator allocator(configuration);

    const auto first = allocator.noteOn(1, 60, 90, 100);
    requireAction(first, RealtimeMidiVoiceActionKind::Retrigger, "initial retrigger");
    require(first->absoluteFrame == 340, "retrigger should carry its delayed attack frame");
    const auto& firstAttack = requireAction(allocator.delayedAttack(0, first->generation, 340),
                                            RealtimeMidiVoiceActionKind::Attack, "matching delayed attack");
    require(firstAttack.note == 60 && firstAttack.velocity == 90 && firstAttack.absoluteFrame == 340,
            "delayed attack should carry current payload at scheduled frame");

    const auto replacement = allocator.noteOn(1, 61, 91, 200);
    requireAction(replacement, RealtimeMidiVoiceActionKind::Retrigger, "replacement retrigger");
    require(replacement->absoluteFrame == 440, "replacement retrigger should carry its delayed attack frame");
    require(replacement->generation != first->generation, "replacement should advance voice generation");
    require(!allocator.delayedAttack(0, first->generation, 340).has_value(),
            "replacement should invalidate stale delayed attack");
    requireAction(allocator.delayedAttack(0, replacement->generation, 440), RealtimeMidiVoiceActionKind::Attack,
                  "replacement delayed attack");
    requireAction(allocator.noteOff(1, 61, 300), RealtimeMidiVoiceActionKind::Release, "release replacement");
    require(!allocator.delayedAttack(0, replacement->generation, 440).has_value(),
            "release should invalidate its pending delayed attack");
}

void testAllocatorRollsBackUnpublishedNoteTransactions()
{
    RealtimeMidiVoiceAllocator allocator({});
    const auto first = requireAction(
        allocator.noteOn(1, 60, 90, 10),
        RealtimeMidiVoiceActionKind::Retrigger,
        "rollback note-on");
    require(allocator.rollback(first), "latest note-on transaction should roll back");

    const auto replacement = requireAction(
        allocator.noteOn(1, 61, 91, 11),
        RealtimeMidiVoiceActionKind::Retrigger,
        "replacement after rolled-back note-on");
    require(replacement.voiceIndex == first.voiceIndex
                && replacement.generation == first.generation,
            "rolled-back note-on should restore voice age and generation");

    const auto release = requireAction(
        allocator.noteOff(1, 61, 12),
        RealtimeMidiVoiceActionKind::Release,
        "rollback release");
    require(allocator.rollback(release), "latest release transaction should roll back");
    const auto repeatedRelease = requireAction(
        allocator.noteOff(1, 61, 13),
        RealtimeMidiVoiceActionKind::Release,
        "release after rollback");
    require(repeatedRelease.voiceIndex == release.voiceIndex
                && repeatedRelease.generation == release.generation,
            "rolled-back release should restore the held assignment and generation");

    require(!allocator.rollback(replacement),
            "an older action must not roll back a newer allocator generation");
}

void testAllocatorNoteHandlingDoesNotAllocate()
{
    RealtimeMidiVoiceConfiguration configuration;
    configuration.polyphony = 4;
    RealtimeMidiVoiceAllocator allocator(configuration);
    require(allocator.ready(), "allocation fixture should be ready");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_relaxed);
    const auto first = allocator.noteOn(1, 60, 90, 10);
    const auto second = allocator.noteOn(1, 61, 91, 11);
    const auto attack = allocator.delayedAttack(first->voiceIndex, first->generation, 12);
    const auto release = allocator.noteOff(1, 60, 13);
    const auto rolledBack = allocator.rollback(*release);
    countAllocations.store(false, std::memory_order_relaxed);

    requireAction(first, RealtimeMidiVoiceActionKind::Retrigger, "allocation first note-on");
    requireAction(second, RealtimeMidiVoiceActionKind::Retrigger, "allocation second note-on");
    requireAction(attack, RealtimeMidiVoiceActionKind::Attack, "allocation delayed attack");
    requireAction(release, RealtimeMidiVoiceActionKind::Release, "allocation note-off");
    require(rolledBack, "allocation-free rollback should succeed");
    require(allocationCount.load(std::memory_order_relaxed) == 0, "prepared allocator note handling should not allocate");
}

void testAllocatorFailsClosedBeforeAuthorityWraparound()
{
    RealtimeMidiVoiceAllocator generationAllocator({});
    talas::runtime::RealtimeMidiVoiceAllocatorTestAccess::
        exhaustSlotGeneration(generationAllocator, 0);
    require(
        !generationAllocator.noteOn(1, 60, 100, 0).has_value(),
        "an exhausted voice slot must retire instead of wrapping its exact generation");

    RealtimeMidiVoiceAllocator activationAllocator({});
    talas::runtime::RealtimeMidiVoiceAllocatorTestAccess::
        exhaustActivationSequence(activationAllocator);
    require(
        !activationAllocator.noteOn(1, 60, 100, 0).has_value(),
        "an exhausted activation sequence must reject before wraparound");

    RealtimeMidiVoiceAllocator mutationAllocator({});
    const auto active = requireAction(
        mutationAllocator.noteOn(1, 60, 100, 0),
        RealtimeMidiVoiceActionKind::Retrigger,
        "mutation-exhaustion setup");
    (void)active;
    talas::runtime::RealtimeMidiVoiceAllocatorTestAccess::
        exhaustMutationSequence(mutationAllocator);
    require(
        !mutationAllocator.noteOff(1, 60, 1).has_value(),
        "an exhausted allocator mutation sequence must reject before wraparound");
}

void testResetRetainsGenerationAuthority()
{
    RealtimeMidiVoiceAllocator allocator({});
    const auto old = *allocator.noteOn(1, 60, 100, 0);
    allocationCount.store(0);
    countAllocations.store(true);
    const auto reset = allocator.resetRetainingGenerations();
    countAllocations.store(false);
    require(reset && allocationCount.load() == 0, "reset must retire notes without allocating");
    require(!allocator.rollback(old), "pre-reset transactions cannot roll back the reset");
    require(!allocator.delayedAttack(old.voiceIndex, old.generation, 5), "reset cancels delayed attacks");
    require(!allocator.noteOff(1, 60, 6), "reset releases held notes");
    const auto fresh = *allocator.noteOn(1, 64, 90, 7);
    require(fresh.generation > old.generation, "replay must not reuse a voice generation");
    require(!allocator.delayedAttack(old.voiceIndex, old.generation, 8), "old attack cannot address replayed voice");

    talas::runtime::RealtimeMidiVoiceAllocatorTestAccess::exhaustMutationSequence(allocator);
    require(!allocator.resetRetainingGenerations(), "reset rejects mutation authority exhaustion");
    require(allocator.delayedAttack(fresh.voiceIndex, fresh.generation, 9).has_value(),
        "rejected reset preserves held state");
    RealtimeMidiVoiceAllocator exhausted({});
    talas::runtime::RealtimeMidiVoiceAllocatorTestAccess::exhaustSlotGeneration(exhausted, 0);
    require(exhausted.resetRetainingGenerations() && !exhausted.noteOn(1, 60, 90, 0),
        "reset cannot revive an exhausted voice generation");
}
}

int main()
{
    testAllocatorRejectsInvalidConfiguration();
    testAllocatorFiltersChannelAndPitchRange();
    testAllocatorSelectsOldestReleasedVoiceAndLowerIndexTies();
    testAllocatorStealsLongestHeldVoice();
    testAllocatorStacksRepeatedNotesAndReleasesOldestMatch();
    testAllocatorIgnoresStaleNoteOffAfterSteal();
    testAllocatorConvertsVelocityZeroToNoteOff();
    testAllocatorInvalidatesStaleDelayedAttacksByGeneration();
    testAllocatorRollsBackUnpublishedNoteTransactions();
    testAllocatorNoteHandlingDoesNotAllocate();
    testAllocatorFailsClosedBeforeAuthorityWraparound();
    testResetRetainsGenerationAuthority();
}
