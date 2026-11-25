# TimeFrequencyScale low-output spectral evidence

2026-09-27. Downward-pitch (low-output) quality evidence. The existing
quarter-speed source gains three fixtures and native quality evidence; its
bounded status and open acceptance remain unchanged.

## Initial evidence and reproduced failure

The initial 12 profiles use 20/40/80Hz sources, Scale .25/.5, Rate .75 and DC 0/.1.
All pass the predeclared analytical criteria. The lowest 5Hz tone nevertheless
has fitted amplitude .184852 from a .25-amplitude source, close to the existing
Hann-projection floor. No production change was made for that initial result.

The same source/DC cross product was extended to Scale .125/.0625, with all
thresholds retained. Eight-second measurement windows contain whole cycles down
to 1.25Hz and remain inside the source and synthesis support. Four of 24 cases
fail: 20Hz shifted to 2.5/1.25Hz, with and without DC. Hann projections are
.044013/.0402681 against the unchanged .045 minimum; fitted amplitudes are
.176052/.161073. Pitch and DC measurements pass, isolating the gain loss.

## Spectral repair

Map formerly discarded negative translated destinations. Those bins belong to
the shifted lobe even when its requested center frequency is positive. The map
now stores their conjugates in the positive half; the existing mirror restores
the complete real spectrum. Interior coefficients represent conjugate pairs, so
their contribution doubles when a pair merges at DC or Nyquist. Original
endpoint coefficients instead receive half weight when moved to an interior
frequency. Endpoint status comes from the source bin, not its owning peak.

The preserved DC region and existing above-Nyquist crop remain. No new arrays,
stages, latency, allocation or checkpoint state are added. Pending Map snapshots
already retain the complete positive half where folded contributions land.
Independent read-only review found no endpoint-accounting or checkpoint defect.

All 24 profiles pass after repair. The 1.25Hz fitted amplitude becomes .246071
without DC and .245967 with DC. The new tests retain the initial amplitude,
frequency, DC and residual criteria. Three additional cosine-source profiles
move the original Nyquist endpoint into interior frequencies; all recover the
.25 source amplitude and .1 DC, with residual RMS at most 1.12e-12.

## Public runtime and Release verification

The shared test-only reference uses source and sine/cosine mathematics, without
production FFT/DSP dependencies. It also checks finite output and exact startup
silence. Three 20Hz profiles cover Scale .0625/DC .1, .125/DC 0 and .25/DC .1.
Offline and prepared 31/128-frame outputs each face the independent oracle.
Prepared runs inject invalid Rate 0 at frame 200817, at offsets 30/113. The whole
call becomes silent, the repaired Rate retries exactly, and each full timeline
matches offline output. The 128-frame path resets without allocation and replays
the full render, including failure and correction.

All **36/36 new measurements** pass in Release: 24 low-output DSP cases, three
original-endpoint DSP cases and nine public runtime measurements. Across them,
Hann projections range .0578688–.0625, all scanned pitch offsets are 0Hz, maximum
mean-DC error is 5.90431e-6 and maximum residual RMS is .000879535. Complete
callback, reset and recovery comparisons also pass.

All six complete Release suites pass in 60.55s: PhaseVocoder 1.37s, source-window
DSP 40.90s, checkpoints .56s, live recording 1.79s, file source .72s and
quarter-speed runtime 15.20s. Earlier modulation, source selection, incremental
FFT, pending-copy, low-tone/DC, latency and rollback gates remain enabled.

## Debug and final integration verification

All **12/12 Debug suites** pass in 282.27s. Besides the six Release suites, this
includes compilation, offline rendering, prepared scope authority, runtime
conformance, general realtime rendering and resource snapshots. All **36/36 new
measurements** pass again; their printed numerical results agree between builds.
CLI and Workbench build successfully, with only the previously recorded
duplicate-library linker warnings.

Generated catalog, capability matrix and node help checks pass. All native
processes have completed.

## Retained diagnostics and limits

Logs are in registered `build/workbench-debug`, with prefix
`time-frequency-scale-low-output-`:

- `red-build.log`, `red-probe.log`: the initial 12-case PASS; the planned RED
  filename is not evidence of a failure.
- `extended-build.log`, `extended-red.log`: the actual four failing cases.
- `repair-build.log`, `repaired-probe.log`: the initial repaired 24-case result.
- `release-build.log`, `release-tests.log`: full Release integration and all
  36 new analytical measurements.
- `debug-build.log`, `debug-tests.log`: Debug product build and regression run.
- `generated-check.log`: final generated-artifact freshness checks.

No new hardware deadline measurement is claimed for these profiles. Arbitrary
mixtures, output-Nyquist boundary quality, the spectral/raw transition waveform,
and continuous streaming remain open. These native analytical fixtures are
not external reference captures.
