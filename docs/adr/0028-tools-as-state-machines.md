# ADR-0028: A Tool is a state machine the project runs

Status: Accepted (taken as recommended under the owner's standing approval
of 2026-10-12)

## Context

A Tool is a state machine with global and local variables, whose states
hold triggered responses written in Smalltalk. Its widgets, windows and
editor are user interface, but the machine is behaviour that can be checked:
which responses run on a step, in what order, when the machine moves, when
it steps, and what it writes to Event Values. None of it existed here.

## Decision

`talas::project::tool::Machine` runs a Tool's `Definition` without a window.

- **States and responses.** A state holds triggered responses, a condition
  (`onEntry`, `onExit`, `true`, or a Smalltalk test) and a response. On a
  step the machine tests every condition of its state in order, then runs
  the responses whose conditions held, in order. A response whose
  last value is a Symbol naming another state moves the machine at once:
  the state's `onExit` responses run, then the new state's local variables
  are set and its `onEntry` responses run; the rest of the step's
  responses do not. A Symbol naming the state it is in goes on to the next
  response. `onEntry` and `onExit` never move it.
  `TerminalState` stops the Tool. Testing each condition just before its
  response was the alternative; collect-then-run is the one taken.
- **Variables.** A name beginning with an uppercase letter is global, one
  beginning with a lowercase letter is local to its state; an
  assignment to an undeclared name is refused. A variable may step the
  machine when it changes from outside, write or read an Event Value
  (`eventWrites`, `readEventValue`), and constrain a number to a grid,
  then a range. `self postAllEvents` sends every Event Value the
  variables write.
- **Pace.** `StepIntervalInMilliseconds` steps a self-running Tool once per
  interval as time advances, as fast as it is asked when 0, and never on
  its own when negative.
- **Objects between steps.** The responses run in one
  `talas::project::smalltalk::Session`, whose interpreter keeps its closures,
  collections, streams and random number generators from run to run, so a
  Random or a block kept in a variable works on a later step. A block runs in the program it was written in.

## Amendment: what a variable represents (2026-10-23)

A variable's `Kind` is a Number, a String or an Object, each
holding what Initially gives; a Random number stream, seeded by Initially
from 0 to 65535 and seeded again when its state is entered again; a Text
file; or a Compiled Sound Grid.

- **Text files** live in the machine's memory: the environment hands it a
  file's contents (`setTextFile`) before the Tool runs and reads them back
  (`textFile`) to save. A Text file variable answers the read and
  write stream messages as its access lets: Read only, Write only, Append
  after the file's contents, Read/Append, or Read/Write of a file emptied
  as it opens. Reads go on from the start of the file, writes at its end.
  `cr` writes the line end of a text file, `\n`.
- **Compiled Sound Grids** play nothing themselves: `playSoundNumber:`,
  `playSoundNamed:` and their `withSubstitutions:` forms, with a
  BindingList of Associations (`?sample1 -> 'siren.aif'`), and
  `SignalProcessor stop` are recorded as `soundRequests` for the
  environment to carry out.
- **Views** draw nothing themselves either: `View new` set to a View
  variable takes the rectangle the environment lays out for it
  (`setViewBounds`), and its `graphicsContext` records each line, string,
  rectangle, dot, arc, polygon and Polyline with its paint and line width
  (`drawings`) until it is cleared. Rectangles, origin to
  corner with y growing downwards, are values of the Smalltalk subset.
- Any object answers `printString` and `printOn:`, Times and
  Dates printed as VisualWorks prints them.
- `MinimumWindowExtent` and `MaximumWindowExtent` give the size the Tool
  opens at, halfway between them (`openingExtent`). A state's
  `onEntry` and `onExit` must come first among its responses.

A Tool's session runs for as long as the Tool does, so it may not run out
of objects: Points, Rectangles, complex numbers, Dates, Times, Filenames and
Associations carry their parts in the Value itself, and a block slot no
Value holds any more is used again, as is the place of a collection,
stream, Dictionary or Random no Value holds. Programs are parsed once per
text. The Tool's files are a `smalltalk::MemoryFileSystem` its scripts
reach too, so `'log.txt' asFilename appendStream` writes the file a Text
file variable reads.

## Amendment: a first-true step rule and Views that step (2026-11-15)

A step may run only the first block whose condition is true, then update
the state, instead of every such response. A
Tool's `Definition::stepRule` chooses: `EveryTrueResponse` stays
the default, and `FirstTrueResponse` still tests every condition but runs
only the first true one's response. The Tool editor's "First true
response only" box sets it, and the Tool file keeps it as `stepRule`,
written only when it is `FirstTrueResponse`, so older files read as before.

A response that sets a View variable marked Step When Changed to a new
View steps the machine: the response ends at that assignment, the
rest of the pass (the state's other onEntry responses, or the step's
remaining responses) is not run, and the machine steps once the run in
hand has ended. Setting a View variable to the View it holds is no change,
and Views without Step When Changed, or set from outside, step nothing.

## Consequences

The Tool's behaviour is tested headless (`talas_tool_tests`). A Tool is not
yet part of a project document, so nothing saves, opens or plays one; its
widgets, the painting of its Views, the files on the disk and the Sounds a
grid plays, and the Tool editor, remain to be built on the machine.
