# Built-in wavetables

Talas provides a set of standard wavetables. Type one of these names into a resource
field, such as an Oscillator's Wavetable or a GrainCloud's GrainEnv, and the
table plays without a file in the project:

| Name | Shape |
|---|---|
| `Sine`, `Cosine` | one cycle |
| `Triangle` | one cycle, rising to 1 at a quarter and falling to -1 at three quarters |
| `Saw21` | a rising sawtooth of its first 21 harmonics, each 1/n of the first |
| `Square21` | a square of the odd harmonics up to the 21st, each 1/n of the first |
| `Buzz` | a pulse of the first 21 harmonics, equal and in cosine phase, 1 at its start |
| `GlottalPulse` | one opening (40% of the cycle) and closing (16%) of the glottis, then closed |
| `FullRamp` | a straight line from -1 up to 1 |
| `Ramp` | a straight line from 0 up to 1 |
| `Gaussian` | a bell from near 0 up to 1 and back |
| `Exponential`, `ExponRev` | a rise from 0 to 1 over 60 dB, (1000^x - 1) / 999, and the same falling back |
| `FourExpons` | four ExponRevs in a row |
| `Random` | uniform from -1 to 1, the same each time |
| `ATAN20`, `ATAN40` | Waveshaper curves of an input from -1 to 1, atan(20 x) and atan(40 x) scaled to reach 1 |
| `LinearEnvelope` | a straight rise from 0 to 1 and fall back |
| `Hann`, `Hamming` | windows; Hann is 0 at its first and last entries |
| `Rectangular` | 1 throughout |
| `Zero` | 0 throughout |

Each table has 4096 entries, as the Oscillator expects. The tables are
computed from their definitions, so Gaussian's
width, Buzz's 21 harmonics, GlottalPulse's shape (the TwoFormantElement's
pulse), the exponential and arctangent curves and Random's seed are this project's choices; Saw21, Square21 and Buzz are scaled to peak
at 1. A project resource or host table with the
same name replaces the standard one. Recorded or analysed tables, samples and
spectra are not built in; add them to a project as resources.

`talas_system_wavetables_tests` checks the shapes and harmonics, plays an Oscillator
and a GrainCloud on them offline and in realtime, and loops a Sample on one.
