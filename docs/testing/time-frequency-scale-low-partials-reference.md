# TimeFrequencyScale nearby-partial counterexample

2026-09-27. This applies predeclared simultaneous-partial criteria to the
downward-pitch path covered by the
[low-output evidence](time-frequency-scale-low-output-reference.md).
This document preserves the original native quality failure and its diagnostic
evidence. Later component synthesis now passes the unchanged **64/64 profiles
in both Debug and Release** with N8192/H2048/D10239. See the
[integration evidence](time-frequency-scale-component-synthesis.md) for that
repair and its still-open callback timing work. The historical results below
are retained; they describe the earlier implementation.

A native analytical failure is a failure against Talas's own predeclared
oracle, not an observed mismatch with an external reference. The existing
quarter-speed source remains bounded, with its acceptance unchanged.

## Oracle and controls

Two sources contain 20/40Hz or 20/28Hz partials. Cross Scale .0625/.125, Rate
.25/.75, DC 0/.1, amplitudes .25/.08 in both orders, and source phase pairs
(0,0)/(.73,-1.1): 64 profiles. N8192 at 48kHz gives native delay 10239. The
nine-second recording, eleven logical output seconds and final eight-second
measurement keep source/synthesis support interior. Both requested frequencies
have whole cycles over the measurement interval.

The shared reference contains source and projection mathematics only. It checks
each partial's pitch, absolute gain, relative gain and rejected rival frequencies,
then fits both sine/cosine pairs independently to measure the residual. DC,
finite output, exact silent startup and callback allocation are checked as well.
The numerical bounds were written before the first production probe.

Before that probe, the local frequency scan was restricted to half the spacing
to the other requested partial; a stronger legitimate neighbor must not win a
quiet tone's scan. Ideal 1.25/1.75Hz mixtures pass in both amplitude orders. An
upper-partial gain of 1.26 is correctly rejected despite being inside the
individual .72..1.28 gain bounds. Both builds pass these four oracle controls.
Independent read-only review found no measurement or source-support defect.

## Reproduced native failure before component synthesis

Both Debug and Release pass **34/64 profiles (53.125%)** and fail the same 30.
All 32 of the 20/40Hz profiles pass. Their partial gains range .931961–1.03532,
and maximum residual RMS is .00304008. Only two of the 20/28Hz profiles pass.
Their quiet-partial gain can fall to .00025302, and maximum residual RMS reaches
.151715. This is substantial component loss, not merely a marginal tolerance.

At N8192 the source bins are about 5.86Hz apart; the nearby tones are only
1.37 bins apart. The Hann lobes overlap heavily. The current local-maximum
detector and nearest-peak phase ownership can assign a mixture to one dominant
peak. Assigning a shared bin to a different owner cannot generally recover both
overlapping components. This source-resolution interpretation is supported by
the following controlled window diagnostic.

## Larger-window diagnostic

The diagnostic keeps the same source, logical measurement window and numerical
criteria, changing only the explicit prepared window and its native delay.
Source support remains inside the recording. It reports every case and is not
an alternative acceptance gate for the failing N8192 profile.

| Window | Delay at 48kHz | Close-pair passes | Partial gain range | Maximum residual RMS |
| --- | --- | --- | --- | --- |
| 8192 | 213.31ms | 2/32 | .00025302–1.00282 | .151715 |
| 16384 | 426.65ms | 16/32 | .409870–1.00634 | .0369597 |
| 32768 | 853.31ms | 32/32 | .970128–.998771 | .000689256 |

No public preparation, source support, latency, capacity, FFT staging or state
copy behavior was changed. The larger-window result does not close the current
profile's failure. A preparation change would also need changing-source,
transient, source-availability, rollback and callback-cost evidence.

MinInputFreq is the expected lowest input frequency and Detectors is a
tracking-sensitivity control, with 10 as a starting point. Neither fixes an
FFT size, minimum partial spacing or a numerical tracking algorithm, and this
native probe does not define them.

## Original verification status and retained logs

Both targeted DSP builds pass. Both `--low-partials-probe` runs exit 1 with the
same known counterexample, and every printed numerical result agrees between
builds. The Release `--low-partials-window-probe` completes
and reports the table above; its exit code indicates diagnostic completion,
not that every window passed. Production code is unchanged, so no broader
native regression or product rebuild is claimed for this package. Earlier
passing packages remain historical evidence, not a green result for this new gate.
Generated catalog, capability matrix and node help freshness checks pass.
Those metadata checks do not close the DSP failure.

All logs are under registered `build/workbench-debug`, with prefix
`time-frequency-scale-low-partials-`:

- `baseline-build.log`, `oracle-build.log`, `baseline-probe.log`: original
  preparation, validated oracle and first actual counterexample.
- `window-build.log`, `window-probe.log`: larger-window diagnostic.
- `debug-build.log`, `debug-probe.log`: independent build and reproduced failure.
- `generated-check.log`: final metadata freshness checks.

Next investigate component separation within the existing analysis window, using
a bounded offline prototype before changing the realtime processor. Preserve the
original 64-case failure and every threshold. Any production candidate must
retain source timing, changing-source behavior, allocation-free processing and
complete rollback; a larger window alone is a quality/latency tradeoff requiring
explicit verification. The spectral/raw blend, arbitrary spectra, continuous
streaming and agreement with an external reference also remain open.

Follow-up: the [offline component estimator](time-frequency-scale-component-estimator.md)
now separates all5984 consecutive clean source windows and all108 input-precision
and noise profiles under fixed accuracy gates. This demonstrates source-model
feasibility without a larger window. It does not change the production results
or close the native acceptance failure above.
