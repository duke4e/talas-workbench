#pragma once

#include "talas/project/ProjectDocument.h"

#include <optional>
#include <span>
#include <string_view>

// In Val an Event Value's name is not case
// sensitive, !SnailsburyTails + !snaiLsBuryTailS being one Event Value,
// named in the Virtual Control Surface as it was first seen.
namespace talas::project
{
// The document with each Event Value written as its first spelling: a
// built-in one's own (!keynumber is !KeyNumber), then a spelling in
// `firstSeen`, as the environment's Event Values, then the Virtual Control
// Surface's, the Timeline's and the first an expression writes, in the
// order of the graph and then of its Sound definitions. Nothing when every
// name is already so spelled or the project's expressions are not Val,
// whose frozen profiles keep their names' case.
[[nodiscard]] std::optional<ProjectDocument> withEventValueSpellingsUnified(
    const ProjectDocument& document, std::span<const std::string_view> firstSeen = {});

// A Val project before format 113, whose names differing only in case
// were different Event Values, with each such name but the first written
// apart by a number, !a and !A becoming !a and !A_2, so that they stay
// different; true when it wrote any.
bool keepEventValueSpellingsApart(ProjectDocument& document);

// Whether two Virtual Controls of a Val project name one Event Value,
// differing only in case; the second's index when they do.
[[nodiscard]] std::optional<std::size_t> findEventValueSpellingTwin(const ProjectDocument& document);

// Format 116: a Val document with each !KeyPitch bent by the pitch wheel,
// (!KeyPitch + !PitchBend nn), and each !KeyPitch12 by an octave, and so
// marked as one whose !KeyPitch is the key alone. Nothing for a document
// already so marked, of another language, or reading neither.
[[nodiscard]] std::optional<ProjectDocument> withKeyPitchBends(const ProjectDocument& document);

// Whether a Val document reads !KeyPitch or !KeyPitch12, in any case.
[[nodiscard]] bool readsKeyPitch(const ProjectDocument& document);

// Format 117: a Val document with each !Random and !Random1000 drawn every
// millisecond, (1 ms random), and so marked as one whose !Random is an Event
// Value of its own. Nothing for a document already so marked, of another
// language, or reading neither.
[[nodiscard]] std::optional<ProjectDocument> withRandomEventSources(const ProjectDocument& document);

// Whether a Val document reads !Random or !Random1000, in any case.
[[nodiscard]] bool readsRandomEventSources(const ProjectDocument& document);

// Format 118: a Val document with each neg:zero:pos: starting at 0, written as
// neg:zero:pos:initially: 0, and so marked as one whose neg:zero:pos: selects
// at once. Nothing for a document already so marked, of another language, or
// holding none.
[[nodiscard]] std::optional<ProjectDocument> withSignSelectionsStartingAtZero(const ProjectDocument& document);

// Whether a Val document holds a neg:zero:pos: without initially:.
[[nodiscard]] bool holdsSignSelections(const ProjectDocument& document);

// Format 119: a Val document marked as one whose time functions run on through
// branches not taken, each expression with a conditional and state so marked.
// Nothing for a document not so marked, of another language, or holding none.
[[nodiscard]] std::optional<ProjectDocument> withTimeRunningOnInSkippedBranches(const ProjectDocument& document);

// Whether a Val document holds an expression with both a conditional
// and state.
[[nodiscard]] bool holdsTimeInSkippedBranches(const ProjectDocument& document);

// Format 122: a Val document whose expressions reading !RealTime or !Time are
// marked to read the whole Sound's clock. Nothing for a document whose
// !RealTime and !Time are Event Values of their own, of another language, or
// reading neither.
[[nodiscard]] std::optional<ProjectDocument> withWholeSoundClocks(const ProjectDocument& document);

// Whether a Val document reads !RealTime or !Time, in any case.
[[nodiscard]] bool readsTimeEventSources(const ProjectDocument& document);

// Whether a document holds an expression that is a function of time.
[[nodiscard]] bool holdsTimeFunctions(const ProjectDocument& document);
}
