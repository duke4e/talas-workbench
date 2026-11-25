# ADR-0015: Bounded Generative Language and Broader Script-Runtime Deferral

Date: 2026-08-15

Status: Accepted bounded C7 boundary; broader script runtime deferred under the
owner's standing auto-approval

## Context

C7 introduced four deliberately closed authoring stages: a deterministic
context-free generator, bounded Timeline and Score output, a compile-time safe
parameter transformer, and restricted catalog-derived reusable-Sound graph
construction. Each stage has explicit syntax, data, time, memory, identity,
and publication bounds. None executes arbitrary object messages or obtains
ambient host authority.

The catalog also contains historical `talas.contextFreeGrammar`,
`talas.limeInterpreter`, and `talas.script` node identities. They preserve narrow
compatibility behavior for existing projects, but their descriptors remain at
`Placeholder` maturity. Their names do not prove compatibility with
Smalltalk, the Lime notation program, or a general script language.

C7 Step 5 asks whether the accepted bounded stages justify promotion to a
broader script language. They do not. A general runtime would create a new
parser and VM authority, mutable-state and voice-lifetime model, persistence
and migration contract, realtime scheduler, capability sandbox, debugger and
diagnostic surface, and security boundary. No current accepted product case
requires those costs or risks.

## Decision

The bounded C7 language family is accepted as the product's generative
authoring boundary. A broader Smalltalk-style or arbitrary script runtime
is deferred.

The following rules are normative:

1. `talas.contextFreeGrammar`, `talas.limeInterpreter`, and `talas.script` remain
   compatibility placeholders. Their existing project bytes and type IDs keep
   their historical meaning and are never reinterpreted as a future language.
2. Their capability descriptors publish the stable limitation
   `arbitrary-script-runtime-unsupported` in addition to the existing exact
   offline and prepared-realtime subset limitations.
3. Placeholder script-family nodes cannot enter restricted generated graphs.
   The shared catalog validator is the authority used by the planner,
   ProjectEditor boundary, and Workbench picker.
4. The C7 generator has no direct filesystem, network, subprocess, device,
   credential, native-call, dynamic-loader, plugin, project-replacement, or
   callback-execution capability.
5. Generated events reach runtime only through the existing typed bounded
   scheduler. Generated Sounds are ordinary validated project definitions and
   become executable only after explicit ordinary instantiation and staging.
6. No C7 acceptance evidence grants trust or execution authority to external
   code. ADR-0014 remains the separate isolation and capability-policy
   boundary.

## Future reconsideration gate

A broader language may be proposed only under a new ADR and implementation
plan that defines and proves all of the following before product activation:

- a new versioned language and bytecode/VM identity, without legacy-byte
  reinterpretation;
- closed parser, compiler, instruction, recursion, allocation, memory, event,
  and wall-time budgets;
- deterministic seed/time semantics and cancellation at every bounded phase;
- project, definition-instance, voice, and session state ownership, including
  reset, snapshot, migration, replacement, and rollback behavior;
- an explicit capability set with filesystem, network, subprocess, device,
  credential, native-call, and dynamic-loading access denied by default;
- prepared-realtime admission and fixed callback work, or an honest
  offline-only disposition;
- exact diagnostics, fuzzing, property tests, sanitizer evidence, migration
  fixtures, package truth, and independent security review; and
- a demonstrated product workflow that cannot be served by the accepted C7
  bounded languages.

Meeting only part of this list cannot promote a placeholder or imply general
compatibility.

## Consequences

Users receive truthful catalog and help metadata instead of an implied
arbitrary runtime. The generator UI stays small, deterministic, auditable, and
safe to publish as one bounded edit. Existing legacy projects retain their
current compatibility behavior. Future language work remains possible, but it
must enter through an explicit versioned security and migration gate rather
than growing accidentally from placeholder names.

Full Smalltalk compatibility is not implied.

## Amendment (2026-10-03): ContextFreeGrammar

Owner decision 2026-10-03. ContextFreeGrammar generates a Sound of Concatenations and CenteringMixers of its
Inputs from a StartExpression by rewriting with weighted production rules,
a Seed making the result repeatable, MaxRewrites bounding the rewriting and
MaxSize the number of Sounds. Its result is fixed when the Sound is
compiled, so it is a bounded compile-time expansion, not a script runtime,
and rule 1 above no longer covers `talas.contextFreeGrammar`.

- Its `inputs` port takes any number of Sounds (ADR-0017); in the order
  they are connected they are the terminals s1 to sn.
- RewriteRules hold rules of the form `A -> (expression) [weight]
  | ... .`, with single uppercase letters as variables, `,` for a
  Concatenation and `+` for a CenteringMixer in fully parenthesized
  expressions.
- The StartExpression is rewritten, leftmost variable first, choosing each
  option by its weight with a generator the Seed starts, until no variable
  is left, MaxRewrites rewrites have been made or the expression holds
  MaxSize Sounds; variables left then are silent. Seed, MaxRewrites and
  MaxSize must be fixed whole numbers; at most 1024 Sounds are generated.
- Compiling replaces the grammar with its result: each Sound is its own copy
  of the Sounds feeding the Input it names, a Concatenation starts each part
  when the one before it ends, and a CenteringMixer starts each part so that
  the parts' midpoints line up, both through TimeOffsets that start their
  Inputs late (ADR-0023), and a Mixer at the grammar's Left and Right levels
  sums them. The Inputs' own Sounds leave with the grammar unless something
  else hears them.
- A part whose duration is not known when compiling ends its Concatenation
  there, as an OverlappingMixer does, and starts with the others in a
  CenteringMixer. Inputs that hold feedback loops are refused.

A grammar that generates nothing stays silent, as the placeholder was, so
older projects keep their sound; format 65 drops the placeholder's text
Inputs field. The weighted choices come from Talas's own random generator.

## Amendment (2026-10-12): Variables a Script gives a Sound

Owner decision 2026-10-12. A Variable is a placeholder for a single
Sound. A Script gives it a Sound as it gives
an Input's variables their values, so the bounded compile-time Smalltalk subset that runs a Script (owner,
2026-10-06) grows by:

- An event's keyword whose value is one of the Script's Inputs gives that
  Input's Variable of the same name (its VariableId) the Sound: in the
  event's copy of the Input the Variable is replaced by a copy of the
  named Input's Sounds. `orchestra at: n` and `inputs at: n` name the n-th
  Input. An Input sent keywords without `start:` is that Input given
  values, a Sound for a Variable rather than an event (as in
  `aFilter start: 1 s input: (aNoise dur: 1 s)`); it starts with the event
  that uses it.
- This is still compile-time expansion: the script runs once as the Sound
  compiles, the copies are ordinary graph nodes, and Inputs holding
  feedback loops or other Scripts are still refused (other Scripts allowed
  since the 2026-10-18 amendment).

Outside a Script a Variable plays its declared selection as its default
Sound, and live selection between declared Sounds is a Talas addition; a
Variable with no declaration and nothing given plays silence. Older projects compile as before: such scripts and
undeclared Variables were refused.

## Amendment (2026-10-22): strings and Arrays a Script gives

Taken as recommended under the owner's standing approval of 2026-10-12.
A parameter's value in a Script may be any Smalltalk object, so an event's keyword may also give:

- a string, which a resource field holding only that variable (`?name` or
  `@name`) plays, as `wavetable: 'Saw21'`; a string naming one of the
  standard system wavetables resolves as the resource would;
- an Array of plain numbers, written into the expressions reading the
  variable as a literal Array, as `3 of: ?weights` given `weights: #(1 1 1
  4)`.

An event leaves the strings it does not give as its Input's previous
event gave them, as it does numbers. Not given: symbols, Arrays of other
values, and text fields such as a nested Script's (an inner Script
expands before the outer one runs).

## Amendment (2026-10-12): LimeInterpreter stays a placeholder

Owner decision 2026-10-12. LimeInterpreter is meant to read binary files of
Lippold Haken's Lime notation program through a Smalltalk Script. Talas has
no specification of the Lime binary format or of the messages a Lime script
sends to the file, and no Lime file to test with, so rule 1 keeps covering
`talas.limeInterpreter`: it plays its Left and Right levels, and a Lime file
is refused with a diagnostic. A reader of MIDI files exported from Lime, or
a real Lime reader, would need a new amendment and sample files.

## Amendment (2026-11-13): a LimeInterpreter without a Lime file plays its Script

Autonomous decision under the 2026-11-13 goal, narrowing the 2026-10-12
amendment. Apart from the Lime file, a LimeInterpreter is a Script: its
Inputs are templates the Script names, each name a letter followed by
letters and digits (the field refuses others), and Left and Right are its
levels. From project format 149 a LimeInterpreter's Inputs are Sounds
connected to its `inputs` port, as a Script's are; with no Lime file it
plays its Script as a Script does, and an Input with an illegal name is
refused. A Lime file is still refused as before, and a blank
LimeInterpreter still plays its Left and Right levels. An older project's
Inputs text field, which the runtime accepted only blank, is dropped.

## Amendment (2026-10-18): a Script in a Script

A Script can be an Input to another Script. Scripts expand innermost first: a Script heard by another
becomes its Sounds, a Mixer that keeps the Script's name, before the outer
one runs, so the outer's events copy those Sounds, each copy started at its
event's time plus the inner event's own. A variable the inner Script's
events leave unset in its Input takes the outer event's value; one the
inner events set keeps theirs. The inner script runs once, so a
variable (`?name`) in its text keeps its declared value rather than one an
outer event gives. Inputs holding feedback loops are still refused, and a
ContextFreeGrammar inside another still is.

## Amendment (2026-10-19): further script prototypes

Taken as recommended (standing approval 2026-10-12). Six further script
prototypes join the catalog, each becoming as compiling begins what it
plays, as Script and ContextFreeGrammar do:

- TextFileInterpreter and
  MIDIFileInterpreter are Scripts whose script also
  reads a file, a binary resource, through `file`: a text file's atEnd,
  linesInFile, maxValue (-infinity with no numbers), minValue, nextNumber
  (0 at the end or at a word), nextParameters (the next line as an Array of
  numbers and Strings) and reset; a MIDI file's nextEvent, atEnd and reset,
  a note on with its note off one event, answering channel, time and
  duration (microseconds), noteNumber (in units of pitch), velocity, the
  status Symbol, the is... tests and values of controls, programs, pressure
  and the pitch wheel (to within 2048), and the last control and pitch
  wheel values as read, and (2026-10-19, later) a note's pitch wheel
  points (`pitchWheelPointsForChannel:sensitivityInHalfSteps:ifNone:`,
  each an Array of its time in microseconds and its value in half steps,
  from the note's start to its note off, or from another event to the end
  of the file), `ifNone:` taking a value or a block, which is evaluated
  when there is no value. Field names in Strings
  (a wavetable's name) cannot reach resource fields.
- CellularAutomaton plays generations of an elementary
  cellular automaton, Wolfram's rule numbering, zeros past the ends, the
  first Input the initial state's leftmost digit, each generation a chord
  of the Inputs whose cells are on, as long as the longest Input (one that
  plays on taking a second);
  RhythmicCellularAutomaton reads each generation as a
  rhythm of its Input, a step of its duration; both build a Mixer of copies
  started late through TimeOffsets.
- RandomSelection chooses Iterations Inputs by their
  normalised Weights, the first at once and each next after an
  exponentially distributed delay of mean AverageDelay, with Park and
  Miller's generator seeded by SampleSeed and DelaySeed (Talas's own
  sequence).
- Preset plays its Input, its EventValues entries (`!Name channel:
  n; initialValue: v.`) setting those Event Values' starting values;
  the Set to Current Event Values button is not offered.

They are catalogued as utilities.

## Amendment (2026-10-21): four mixing prototypes

Taken as recommended (standing approval 2026-10-12). Four mixing
prototypes join the catalog the same way, becoming as compiling begins what
they play:

- Attenuator multiplies its Input's left and right channels by
  Left and Right, from -1 to 1: a Mixer of its one Input.
- Gain does the same with Left and Right of any value, to boost
  its Input above 1: a Level, or for a stereo Input a Level a channel,
  joined.
- CenteringMixer and EndTogetherMixer mix their Inputs
  with each started late, through TimeOffsets, to line up its duration
  midpoint with the longest Input's or to end with it. An Input of no
  known duration starts at once and is not the longest.

A stereo Input connects its left channel to audioIn or inputs and its right
to rightIn or rightInputs, as the Mixer's do; channels from two Sounds, or
delayed apart, pass through a ChannelJoin so the Mixer pairs them. Each
lasts its longest Input. With them, project format 102 lets the Mixer
last its longest Input too, for the Sounds that read
their Inputs' durations; older projects with one beside such a Sound keep
its duration unknown (`manyInputMixerDurationsUnknown`).

Later the same day, Concatenation joins them: its
Inputs one after another, each starting when the one before it ends,
built as RandomSelection's are, a Mixer of copies started late, so an
Input may come again. One of no known duration ends the sequence, as in a
ContextFreeGrammar; a Concatenation lasts its Inputs together. The
prototypes now expand in rounds, those that play copies of their Inputs
first, then the mixing prototypes, then the Concatenations, inner ones
first, each round looking again so that one copied into another's copies
expands too.

A ParameterTransformer expands first: a
RhythmicCellularAutomaton a Transformation hears becomes a Concatenation
of TimeOffsets, each holding a copy of its Input, and Concatenations a
Transformation hears expand only after it runs, timed by the durations it
gave their Inputs; their new Sounds take the expansion identity of the
Concatenation they come from. A Transformation sending snd
doNotExpandFirst runs on the Sounds as authored, before anything expands.
