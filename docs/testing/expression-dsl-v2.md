# Expression DSL v2 core contract

Talas exposes twelve explicit expression-language versions:

- `LegacyV1` is retained for projects migrated from portable formats 1–22.
- `PureV2` is the frozen pure authority introduced by portable format 23.
- `PureV3` is the frozen format-24 authority and adds the bounded C5 Step 3
  arithmetic-series package.
- `PureV4` is the frozen portable format-25 authority and adds a
  bounded project-owned library of parameterized pure definitions. Project-owned validation,
  discovery, offline preparation, prepared realtime, snapshots, component
  replacement, and Workbench authoring consume the persisted authority.
- `PureV5` is an explicit format-38 pure authority. It retains PureV4 and adds
  bounded `intoXArray(input, [x...], [y...], interpolation)` mapping. Earlier
  expression profiles keep their frozen grammar.
- `SessionV1` is the frozen portable format-26 authority. It retains
  the PureV4 compiler and adds fixed prepared-instance state slots through
  `previous`, `accumulate`, and `sampleHold`. It is intentionally not a pure
  profile and never persists or transfers its runtime cells.
- `VariablesV1` is the frozen portable format-27-through-34 authority. It retains
  SessionV1 exactly and adds only bounded `@variableId` reads resolved through
  the project, workspace session, exact reusable-Sound instance, or exact MIDI
  Voice variable authority. Declaration initial values are portable. Project
  and Sound-instance live cells enter project bytes only through explicit
  capture or snapshot actions, workspace-session cells remain local, and MIDI
  Voice cells are never persisted or captured.
- `VariablesV2` is the default for new portable projects. It retains
  VariablesV1 exactly and adds only the checked Scalar binary selector token
  `rem:` with truncation-toward-zero remainder semantics.
- `VariablesV3` is an explicit format-37 profile. It retains VariablesV2 and
  adds the time-dependent `trigger ramp: duration` selector. Earlier profiles
  continue to reject that syntax.
- `VariablesV4` is an explicit format-40 profile. It retains VariablesV3 and
  adds postfix units on scalar expressions, including `!Interval nn`,
  `(!Length * 2) ms` and `!Level db`. Earlier profiles retain their exact syntax
  and rejection diagnostics.
- `ValV1` keeps VariablesV4's references, functions and state but
  reads fields with Val message syntax and precedence; see
  "Val messages" below. Format 42 stores it.
- `VoiceV1` is an explicit format-36 profile. It retains VariablesV2 and adds
  the exact case-sensitive `?VoiceNumber` token. Each MIDI Voice input graph
  receives the one-based ordinal of its prepared slot. The number remains tied
  to that slot when notes are released, reused or stolen; independent owners
  each begin at one.

The default `talas::expr::parse(source)` call is exactly the legacy parser.
Core-only callers must pass a language explicitly when semantics matter.
Format 23 persists `legacyV1` or `pureV2`. Format 24 adds `pureV3`. Format 25
adds `pureV4` plus the required canonical `expressionDefinitions` array. Format
26 adds `sessionV1` while retaining the same definition schema. Format 27 adds
`variablesV1` plus the required bounded version-1 `variableLibrary` for project
and Sound-definition owners. Format 28 advances that library to version 2 and
adds exact `midiVoice` and local `workspaceSession` owners. Formats 29–34 add
orthogonal Sound-variable, generator, render-artifact, and Live Set state while
retaining the exact selected expression language. Formats 35 and later admit
`variablesV2`; format 37 also admits `variablesV3`, and format 38 admits
`pureV5`. Format 40 adds `variablesV4`. Formats 1–39 migrate in
memory without changing their language
or expression bytes. Every
project-owned parser consumes the persisted authority and no product parser
silently guesses from syntax.

## Bounded-pure fixed budgets

Every `PureV2`, `PureV3`, `PureV4`, `SessionV1`, `VariablesV1`,
`VariablesV2`, `VoiceV1`, `VariablesV3`, `VariablesV4`, or `PureV5` parse is bounded before proportional work:

| Resource | Maximum |
|---|---:|
| source bytes | 4096 |
| lexer tokens | 512 |
| syntax nodes | 256 |
| compiled instructions | 256 |
| syntax nesting | 32 |
| evaluation stack entries | 32 |
| selection-list elements | 32 per list |
| `intoXArray` paired breakpoints | 2–15 per array |
| arithmetic-series elements | 64 conceptual entries |
| project definitions | 32 |
| parameters per definition | 4 |
| definition or parameter name | 64 ASCII bytes |
| aggregate definition source | 32768 bytes |
| definition expansion depth | 8 |
| compiled definition locals | 32 |
| state operator slots | 32 |
| unique event references | 64 |
| unique identifiers | 64 |
| diagnostics | 16 |

A budget failure returns one terminal `BudgetExceeded` diagnostic and no
expression. Accepted V2 syntax is compiled off-callback into one immutable
forward-only instruction program. `tryEvaluate()` uses fixed stack storage,
does not recurse, and performs no allocation, parsing, locking, waiting, I/O,
or ambient-capability access.

`LegacyV1` deliberately retains the pre-C5 parser and recursive evaluator
semantics, including previously accepted format-22 expression sizes and exact
invalid-character ranges. It does not claim the V2 fixed budgets. This keeps
migrated portable formats 1–22 byte-compatible after they reopen as format-37
documents carrying explicit `legacyV1` authority.

Workbench shows the active language in Inspector and Context Help. The shared
ProjectEditor upgrade boundary can move LegacyV1, PureV2–PureV4, or SessionV1
authority to `VariablesV1`: it audits every persisted expression and every
effective built-in descriptor default under VariablesV1, reports the first
stable path and exact byte range on failure, and
commits only the language/format boundary as one Undo step on success. It never
rewrites expression source, and a successful upgrade invalidates the previous
prepared stage until a fresh compile succeeds. Undo restores the exact prior
LegacyV1, PureV2–PureV4, or SessionV1 authority. A separate explicit
transaction upgrades only VariablesV1 to VariablesV2 and the current format while
retaining every expression byte and declaration; Undo restores the exact
VariablesV1 document and Redo reapplies the audited boundary. A further explicit
VariablesV2-to-VoiceV1 transaction enables voice templates with the same audit,
Undo/Redo and fresh-stage requirements. The Inspector also offers a separate
VariablesV2-to-VariablesV3 action for timed expressions; it preserves authored
bytes, audits compatibility, commits as one Undo step and requires a fresh stage.
From VariablesV3, the same primary upgrade control offers VariablesV4. The
ProjectEditor also accepts direct VariablesV2-to-VariablesV4 upgrades; both paths
audit persisted expressions and defaults, preserve source bytes and commit one
Undo step. Inspector labels and Context Help identify the selected profile.
Under PureV4, SessionV1, VariablesV1,
VariablesV2, VoiceV1, VariablesV3, or VariablesV4 the Inspector exposes the
bounded definition list and editor. Each successful create, update, or delete
is one Undo step and requires a fresh stage; an in-use definition cannot be
deleted. For a selected SessionV1 expression, Inspector and Context Help expose
its exact state-slot count out of the fixed 32-slot limit, the one-accepted-live-
update clock, and reset/retention semantics. Runtime cells survive ordinary
transport stop/start, reset on seek, graph replacement, device restart, and
voice reuse, and are never project or snapshot state.

## Scalar postfix units (VariablesV4)

A unit can follow a scalar reference, group, function result, selected list
member, variable or stateful result. It binds before arithmetic, so
`60 nn + !Interval nn` adds two note-number values and
`(!Length * 2) ms` applies milliseconds to the grouped scalar. Every operand is
evaluated once. A compiled fixed-stack instruction uses the same checked
conversion as numeric unit literals; it adds no callback allocation or state.

The suffixes are `hz`, `db`, `nn`, `s`, `ms`, `usec`, `m`, `h`, `days`, `beats`,
`samples` and `samp`. For example, `!Level db` with a value near -6.0206 produces
linear gain 0.5. Beats require a positive tempo, and converting samples to seconds
requires a positive sample rate. An already typed operand is rejected; postfix
units do not silently reinterpret one value domain as another. Nonfinite results
and unknown unit names fail with diagnostics. Definition imports retain unit
nodes, local slots and source provenance. Lazy branches and transactional state
keep their existing evaluation and rollback rules.

A number followed by a pitch name is a note number in every language version:
`4 c` is note number 60, `4 re` equals `4 d`, and `sharp` or `flat`
after the name moves the pitch a half step, so `4 c sharp` is 61. The names are
`c d e f g a b` and the syllables `do re mi fa so la ti` (also `sol` and `si`).
In a frequency field `4 a flat` is 415.305 Hz.

Literal parsing, including the existing negative-decibel convention, is retained.
Use parentheses to specify a signed dynamic operand, such as `(-!Level) db`.
Chained units, typed receivers and ambiguous unary forms are not specified
further. The original ten profiles still reject
`!Interval nn` at the same suffix byte range; no project upgrades implicitly.

## Val messages (ValV1)

`ValV1` reads parameter fields as Val messages. A message follows its receiver: unary
messages such as `3 twoExp`, `!Fader cos` or `4 c hz` bind first and cascade
from left to right; binary operators `+ - * / ** // \\` and the comparisons
then apply from left to right with equal precedence, so `1 - 0.5 * 2` is 1;
keyword messages such as `3 vmin: 2` or `-3 neg: 0.1 zero: 0.2 pos: 0.3`
come last, with binary expressions as receiver and arguments. A minus written
before a number is part of the literal, so `-3 abs` is 3. As in Smalltalk, `//` divides and rounds toward negative infinity and `\\`
is its remainder, so `-7 // 3` is -3 and `-7 \\ 3` is 2, while `quo:` and
`rem:` truncate toward zero: `-7 quo: 3` is -2 and `-7 rem: 3` is -1.
A number needs a numeral before its decimal point, `0.5` and not `.5`, as a period not after a numeral could end a
statement; since project format 106 ValV1 refuses `.5` with
"Nothing more expected->", naming `0.5`,
and older projects gain the zero as they open. `!LocalTime` is the time in seconds since the Sound started, a function of
time rather than a fader. `SignalProcessor sampleRate`
and `SignalProcessor halfSampleRate` are the sample rate and half of it as
plain numbers, so a spectrum's fraction of
half the sample rate becomes a frequency with `L * SignalProcessor
halfSampleRate hz`.

ValV1 also takes Smalltalk-style messages
to numbers: `reciprocal asInteger asFloat
floor ceiling sqrtRounded ln degreesToRadians radiansToDegrees arcTan
arcSin arcCos tan sinc floorToTwoPower ceilingToTwoPower even odd negative
positive strictlyPositive`, the keyword messages `max: min: maxAbs:
minAbs: truncateTo: ceilingTo: log: floorLog: between:and:`, the
comparisons `=` and `~=`, and `Float phi` and `Double pi`. Tests answer 1
for true and 0 for false.

Units are messages that convert what a value measures: `hz`, `nn` and `s`
turn a pitch, frequency or period into one another (`4 c hz`, `440 hz nn`,
`10 ms hz`, `120 bpm s`), `samp` counts a time in samples at the sample rate
(`(1.0 / 120) s samp` is 400 samp at 48 kHz), and `removeUnits` leaves the number. A plain number
takes the field's unit, and a frequency in a duration field becomes its
period.

The unary messages are `abs sqrt exp log twoLog twoExp squared inverse
negated sign truncated rounded clipTo01 wrapTo02 cos sin normCos normSin
asLogicValue removeUnits`; `log` is base 10, while the `log()`
function keeps its natural logarithm. The keyword messages are `mod: rem:
raisedTo: quo: vmin: vmax: roundTo: eq: ne: gt: lt: ge: le: threshold:
true:false: neg:zero:pos: of: into: ramp:`. `of:` indexes a literal array
`#(60 62 64)` and truncates a fractional index; `into:` interpolates
linearly through `#({x @ y} ...)` points. `true:false:` and
`neg:zero:pos:` evaluate only the chosen branch. Function calls, `@`
variables, definitions and selection lists remain available.

Functions of time and triggers run on the
expression's millisecond clock: `ramp`, `fullRamp`, `triangle` and their
`repeating` forms, each with or without a duration (`!KeyDown ramp: 2 s`,
`1 repeatingFullRamp: 0.5 s`); `trigger bpm: rate` and
`bpm:dutyCycle:`; `interval random` and `trigger nextRandom`, uniform in
[-1, 1) from a fixed seed per expression so renders repeat, `normal` and
`nextNormal` (around 0, a third of the range a standard deviation, clipped
to [-1, 1]) and `randExp` and `nextRandExp` (exponential with mean 3,
clipped to [0, 24]) and
the `nextRandomWithSeed:`, `nextNormalWithSeed:` and `nextRandExpWithSeed:`
forms, which repeat for their seed; `smooth: duration` and `smoothed`,
which reach a new value along a straight line.

Messages to a trigger, which is on while positive:
`countTriggers` counts from 0 (-1 before the first trigger),
`countTriggersMod:` wraps, and `countTriggersMod:reverse:` counts down
while its second argument is positive; `triggerEvery: n` passes the first
of every n triggers; `asToggle` and `toggleWhen:` turn between 0 and 1;
`gateWhen:` passes the gates begun while its argument was positive;
`durationBetweenTriggers`, `durationBetweenTriggers: n` (n up to 9) and
`durationOfGate` give numbers of seconds; `hasChangedReset:` latches a
change until its argument turns positive; `sustainBy:` holds a trigger
that was on when the pedal went down, as a sostenuto pedal; `nextChaotic:
r` steps x = (3 + r) x (1 - x), from 0.5 or `seed:`, and `reset:` puts in a
random x; `sampleAndHold:` samples its argument on each trigger and
`trackAndHold:` follows it while the trigger is on. `Float pi` is pi.
`swarmFollowFromPosition:velocity:acceleration:friction:` is not
implemented.

EventVariables: in a field program, a temporary
holding `EventVariable new` (or `EventVariable new initialValue: x`) keeps
its value between evaluations. `v <~ e` assigns it and leaves the value;
`v <+ e` assigns it and leaves none. Expressions in parentheses separated by
commas run in turn and leave the last value one of them leaves; `nil`,
`e updateInternalState` and `v readEveryMillisecond` leave none, the last
having its program evaluated once a millisecond. Such a program runs when
its Event Values change and, when time is in it, once a millisecond; its
other temporaries share their expression's state wherever they are read.

A function of time may stand in any parameter. Offline rendering follows it
wherever the node reads the parameter while playing; the realtime renderer
follows it wherever it updates the parameter live, and elsewhere refuses to
prepare, naming the parameter, rather than play its starting value.
`talas_live_parameter_parity_tests` moves every parameter of every catalog node
the prototype harness plays by a repeating ramp, a smoothed Event Value and
a stepped Event Value, and requires both renderers to agree to the sample
wherever both play, except for the listed parameters whose Event Value the
realtime renderer does not yet follow.

`talas_val_tests` checks worked examples of these messages, units and functions.
Project format 42 stores the profile as `valV1`.

## Voice preparation specialization

See the [VoiceNumber authoring guide](../help/voice-number.md) for Inspector
steps and three-voice seed/frequency examples.

`?VoiceNumber * 0.1 + 0.001` gives each prepared slot a distinct static seed.
The compiler specializes an immutable expression copy before evaluating numeric
parameters and preparing each node. Thus a seed or frequency can depend on the
ordinal even when the parameter itself must remain static. The authored source
and reusable definitions survive editing, saving, reopening and Undo/Redo.

Concrete nodes outside a MIDI Voice input graph reject the token. Reusable
Sound templates may retain it until expansion; each reachable instance must
then have one unambiguous voice owner. Authoring audit checks expression domains
with a representative ordinal, while runtime preparation checks every actual
voice. MIDI Voice allocator configuration cannot use its own child ordinal.
Unknown question-mark names and all uses in earlier expression profiles reject.

An unbound template cannot obtain a number through an identifier resolver.
Specialization leaves no runtime name lookup or mutable variable cell. Ordinary
`@variableId` shadowing and `!EventValue` references retain their original scope.
VoiceV1 runtime graphs require the private compiler plan/settings authority;
a public plan copy or modified specialization cannot replace it. This numeric
feature does not add string concatenation or voice-specific resource names.

## Bounded pure operations

LegacyV1 and compiled language profiles also accept the documented
FunctionGenerator time spellings `usec`, `m` (minutes), `h` (hours), `days`
and `samp` (samples). A scalar divided by Hertz yields a duration in seconds,
so `1 / 2 hz` is a half-second period. Sample units require the compiled
render sample rate for conversion to seconds.

V2 retains all legacy literals, units, references, arithmetic, and domain
conversion rules. It adds:

- `abs(x)` preserving `x`'s domain;
- scalar-only `sqrt(x)`, `log(x)`, and `exp(x)` with finite checked results;
- `clamp(x, low, high)` for three compatible values and ordered bounds;
- `map(x, inLow, inHigh, outLow, outHigh)` with compatible input/output
  domains and a nonzero finite input span;
- `<`, `<=`, `>`, `>=`, `==`, and `!=`, below arithmetic precedence, returning
  exact Scalar `0` or `1`;
- `if(condition, then, else)`, where the condition is finite Scalar and exactly
  one branch is resolved/evaluated.
- `[element0, element1, ...][index]`, a non-empty bounded selection list. The
  finite Scalar index must be an exact non-negative integer in range. Exactly
  one selected element is evaluated; a bare list is not a first-class value.

Selection lists may contain nested indexed lists within the existing depth,
node, instruction, and stack limits. Dependency discovery includes references
from every element, but runtime resolvers are never called for an unselected
element. Empty lists, more than 32 elements, missing indices, fractional or
unit-bearing indices, and out-of-range indices fail closed with an exact source
range.

PureV5 adds a native bounded array-mapping function:

```text
intoXArray(input, [x0, x1, x2], [y0, y1, y2], interpolation)
```

Every member and argument is a finite Scalar expression; EventValue members
are reevaluated when their controls change. The two arrays must have equal
fixed length, and X members must remain strictly ascending. Inputs outside
the X range hold the nearest Y endpoint. Interpolation 0 holds the left
breakpoint, 0.5 is linear, and 1 uses piecewise cubic Hermite segments with
zero endpoint slopes and centered interior slopes. Values between these
settings blend continuously. Invalid order, range, domain, or nonfinite
intermediates fail closed. The parser and evaluator use fixed budgets; the
prepared callback allocates nothing. This function spelling, update cadence,
endpoint slopes, and numeric curve are native candidate choices. PureV4 and
all other earlier profiles reject this form.
The profile can be selected in a format-38 project document. Workbench's
Inspector labels an imported PureV5 project, but Workbench does not yet offer
an audited upgrade action into this profile.

PureV3 is a strict superset of PureV2 and adds one stateless, non-first-class
generated sequence:

```text
series(first, step, count)[index]
```

`count` is a finite exact Scalar integer from 1 through 64. `index` is a
finite exact non-negative Scalar integer smaller than `count`; `-0` selects
zero. The evaluator validates index and count before resolving `first` or
`step`, then computes exactly one checked `first + step * index` value.
`first` and `step` must have compatible domains. The conceptual sequence is
never materialized or iterated, and a bare series is invalid. PureV2 continues
to reject `series` as an unknown function so its persisted grammar remains
frozen.

PureV4 is a strict superset of PureV3. A definition has an ASCII identifier,
zero to four named/domain-declared parameters, one declared result domain, and
one bounded PureV4 body. Calls use normal `name(argument, ...)` syntax. The
compiler rejects duplicate/reserved names, wrong arity or domains, cycles, and
over-depth/over-budget expansion. Arguments are compiled left-to-right and
stored once in fixed local slots; bodies are inlined into the root immutable
program. No definition lookup, parsing, recursion, allocation, lock, wait, or
I/O occurs during realtime evaluation.

SessionV1 is the explicit stateful superset. `previous(value, initial)` returns
the prior accepted value, `accumulate(delta, reset, initial)` advances one
checked accumulator, and `sampleHold(value, trigger, initial)` samples only on
a false-to-true scalar trigger. Each static occurrence has one compiler-owned
slot, including separately inlined definition call sites. Ordinary stateless
evaluation rejects a stateful program; the explicit state API copies candidate
cells and commits them only after the complete evaluation succeeds. State is
owned by one prepared runtime scope, starts from declared initial values, and
is neither project persistence nor snapshot state.

VariablesV1 adds `@name` without changing SessionV1 operators. Variable names
never fall back to Event Values or definition parameters. Project-owned cells
are visible from the root and nested Sounds unless shadowed; a Sound-owned
declaration resolves to a distinct cell for every exact instance path. The
callback consumes only prepared numeric handles—no variable name lookup,
parsing, allocation, or locking occurs there.

VariablesV2 adds exactly one operator without changing VariablesV1 behavior:

```text
left rem: right
```

`rem:` is one case-sensitive, unsplittable lexer token. It is left-associative
at the same multiplicative precedence as `*` and `/`; `rem`, `Rem:`, `rem :`,
`%`, and other selectors remain invalid. Both operands must be finite Scalars,
the divisor must be nonzero, and the checked result is `std::fmod(left, right)`
with every zero normalized to positive zero. Unit-bearing operands, non-finite
results, and zero divisors fail closed with the shared exact diagnostic and
source-range authority. AST interpretation and the immutable prepared program
use identical rules. VariablesV1 and every earlier profile continue to reject
the same `rem:` bytes, and VariablesV2 does not admit arbitrary Smalltalk
message sends or any quotient/div/mod family.

VariablesV3 adds one time-dependent selector:

```text
trigger ramp: duration
```

The trigger is a Scalar and duration is a positive time value. A positive
trigger edge starts at zero; while high, the native candidate rises to one
over duration and holds its value when low. Its running Sound clock is
quantized to millisecond ticks independently of the Timeline composition
cursor. Like every function of time it may stand in any parameter the
renderers follow (see Val above). Tick phase, gate and retrigger semantics
beyond this description are not specified.

The four closed owner kinds have deliberately different lifecycle authority:

| owner | prepared identity | live lifetime | persistence and snapshot policy |
|---|---|---|---|
| `project` | one project cell | accepted prepared stage | explicit capture and eligible snapshots may write project-owned state |
| `soundDefinition` | definition plus exact expanded instance path | accepted prepared Sound instance | explicit capture and eligible snapshots may write that exact instance target |
| `midiVoice` | structural MIDI Voice owner, expanded instance path, slot ordinal, slot generation, and stage generation | allocation through release tail; reset on slot reuse | runtime-only; never captured, snapshotted, or written to project/workspace bytes |
| `workspaceSession` | project workspace plus declaration identity | compatible local workspace and accepted stage | acknowledged scalar values persist only in WorkspaceSession v10+; Sound selections use the separate exact-identity WorkspaceSession v11 facet; eligible snapshots may recall into local state without changing the declaration |

Voice assignment requires the exact active slot and generation and rejects
stale allocation, release/reuse, owner, scope, or stage evidence. Workspace
assignment is optimistic nowhere: the local session changes only after the
runtime bridge acknowledges the exact request. Project replacement flushes or
discards local values through the normal WorkspaceSession lifecycle; they do
not enter project, autosave, recovery, package, component, or host-state bytes.
The Workbench Variables section exposes the same owner choices and availability
authority, including active/releasing voice rows and local-session modified
state.

Seconds and samples are compatible only through a positive finite sample-rate
authority. Other mixed domains fail closed. Equality is exact binary64
equality after required time normalization; no implicit epsilon or truthy
non-scalar conversion exists. Every accepted final value must match the
caller's requested domain through the existing closed conversions.

## Local verification

After configuring `core-debug`:

```sh
cmake --build --preset core-debug --target \
  talas_expression_tests talas_expression_property_tests --parallel
ctest --test-dir build/core-debug --output-on-failure \
  -R '^talas_expression_(tests|property_tests)$'
```

The core-fuzz preset replays all expression profiles against the checked-in expression
corpus. Completion additionally requires focused fail-fast sanitizers, the
complete serial Debug suite, tests-off Release smoke, a clean protected-path
check, and exact-commit independent review. GitHub Actions are not part of the
active acceptance boundary.
