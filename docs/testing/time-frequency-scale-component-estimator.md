# TimeFrequencyScale component identifiability experiment

2026-09-27. Offline numerical research for the unresolved nearby-partial
[native counterexample](time-frequency-scale-low-partials-reference.md).
The implementation in `tools/time_frequency_scale_component_probe.py` uses
NumPy; the production audio processor does not call it. Native N8192 output
still passes **34/64 cases (53.125%)**.

## Question and fixed criteria

Can the existing N8192 original-rate window at 48kHz distinguish nearby tones
without increasing native delay? The estimator receives only samples. Source
frequencies, amplitudes, phases, Rate and Scale are withheld from estimation.
All waveform generation and accuracy scoring are separate from the estimator.
The criteria below were fixed before each experiment, including the failed
intermediate ones.

Use the first 6144 samples for frequency and coefficient estimation. The final
2048 validate reconstruction; they never enter the frequency, amplitude, DC or
alias fit. Validation does select the first acceptable model order, so it is
not an untouched final blind test. Require relative RMS at most1e-4 on training
and validation, unit-root error at most1e-4 and full-rank solves with condition
at most1e12. Independent accuracy limits are .02Hz frequency, .2% amplitude and
1e-6 DC. Consecutive windows additionally require source-center phase error
at most1e-4 radians, modulo2pi.

The estimator tries up to eight stationary sinusoidal components. This cap is
an experimental work bound and does not interpret the Detectors control.
Hann guides below1% of the strongest interior peak are omitted. Alias candidates
include those within two training-spectrum bins and the nearest guide assignment
for each root; a hidden component need not have its own local maximum. Enumerate
at most256 combinations and choose by training residual. Reject excess ambiguity,
endpoints that are not handled, and almost-cancelling models whose component RMS
exceeds four times the observed training standard deviation. That last guard is
a confidence heuristic; some legitimate but fragile mixtures may also reject.

## Methods and observed failures

1. **Direct autoregression.** First differences at stride32 remove constant DC;
   least-squares recurrences of orders2..16 produce candidate sinusoid roots.
   The first version accepts184/192 low-partial windows. Unfiltered FFT sidelobes
   can select the wrong alias. A growing-amplitude tone is also falsely accepted
   as two components .00002156Hz apart, each amplitude8651.83, almost cancelling.
2. **Guarded alias selection.** Significant guides and the cancellation guard
   reject all eight negative controls. A hard two-bin alias radius initially
   rejects52/192 windows; retaining the nearest-guide candidate fixes that.
   Clean low-partial windows then pass192/192 and contiguous windows5984/5984.
   Input precision exposes the remaining fragility: no simulated PCM16 profile
   is accepted, and float32/PCM24 each pass only12/18.
3. **Subspace estimate.** Form an approximately95x97 lag matrix from the same
   training differences. Its left singular subspace retains candidate modes;
   eigenvalues of the least-squares one-row shift yield roots. This improves
   input sensitivity to106/108. One PCM16 window fails validation RMS; one noisy
   accepted window misses the independent DC threshold (1.0594e-6 versus1e-6).
4. **Training refinement.** Up to three Gauss–Newton frequency steps, with linear
   DC/sine/cosine refits, remove that frequency-estimation bias. Every step must
   improve training residual, stay within half a training bin, and move at most
   one bin in total. Preserve all earlier gates and rejection controls.

The initial subspace run also found a zero-rank diagnostic serialization defect:
NaN could reach strict JSON. Zero singular values now reject explicitly. Its
failure log is retained. The final command treats warnings as errors and records
the script SHA256 and NumPy version. An inaccurate accepted model fails the
command even when its coverage group permits conservative rejection.

## Final results

| Measurement | Correct / total | Scope |
| --- | --- | --- |
| Original64 profiles at three centers | 192/192 (100%) | Scale duplicates included |
| Every capture center in logical seconds3..11 | 5984/5984 (100%) | 32 unique source profiles,187 centers each |
| Broader stationary tones | 32/36 (88.9%) | Four exact stride32 alias collisions reject |
| Precision and added-noise profiles | 108/108 (100%) | Six input types,18 profiles each |
| Nonstationary/noisy rejection controls | 8/8 (100%) | Chirps, ramp, step, impulse, noise and held-out change |
| Endpoint controls | 2/2 | Constant accepted; original Nyquist conservatively rejected |

The precision groups are double, float32, simulated PCM24, simulated PCM16 and
added noise RMS1e-6/1e-5. Each crosses nearby pairs around20/173/5000Hz, amplitude
ratios .32/.1/.01 and two source centers. Quantization is nearest normalized
signed-PCM step; this is not a disk decoder or public-renderer run. Maximum
frequency error is .000834Hz, relative amplitude error .01721%, and DC error
3.279e-7. Every accepted sensitivity model meets the independent score.

Clean consecutive windows have maximum frequency error1.851e-12Hz, amplitude
error3.185e-13 relative, DC error2.929e-15 and center-phase error4.470e-13 radians.
The separate broader fixtures include nearby pairs through13kHz and one to eight
dispersed tones. The four unsupported cases are64/1564Hz and89/1589Hz at two
centers: stride32 aliases each pair to one mode. Their rejection is not evidence
that the original-rate samples lack information.

## Reproduction and retained evidence

From the repository root, with NumPy available:

```sh
PYTHONDONTWRITEBYTECODE=1 OPENBLAS_NUM_THREADS=1 VECLIB_MAXIMUM_THREADS=1 \
  python3 -W error tools/time_frequency_scale_component_probe.py --stress \
  --output build/workbench-debug/time-frequency-scale-component-estimator-final.json
```

The default is `--method subspace-refined`; `ar` and `subspace` retain the earlier
methods for comparison. Final process success allows the documented conservative
alias rejections; inspect the per-group counts, not just the exit code.
Registered `build/workbench-debug` contains JSON/log pairs with prefix
`time-frequency-scale-component-estimator-`: `initial`, `guarded`, `candidate`,
`stress`, `subspace-complete`, `refined` and `final`. The failed `subspace.log`
records the serialization error; no complete JSON was written for that run.
The initial AR JSON predates strict serialization and contains a NaN diagnostic;
later successful JSON is standard compliant. No native build, render or timing
campaign was run during this experiment because production DSP did not change.

## Remaining implementation work

The experiment establishes identifiability for these signals within the existing
window. It does not synthesize pitch-scaled output or resolve the failing default
native DSP test. NumPy SVD, eigensolvers, least squares and trigonometric matrices
are offline work; no callback budget or allocation guarantee follows from them.

A native candidate needs fixed storage and bounded staged computation, a raw
snapshot captured at the current source-authority instant, stable component
identities and synthesis phase through Rate/Scale changes. Model entry/exit and
fallback must preserve continuity; pending estimator state must join exact
checkpoint, reset and whole-call rollback behavior. Keep the current N8192
preparation, source support, latency and all64 numerical gates while evaluating
that candidate. Broader noisy spectra, transients, alias collisions, raw/spectral
blend quality and agreement with an external reference remain open.

Native follow-up: a [fixed-storage lag-subspace kernel](time-frequency-scale-native-subspace.md)
now passes analytical matrix, independent NumPy and pending-state checks. It
supplies the first numerical stage of a native estimator. Frequency extraction,
reconstruction and integration into the audio processor remain pending.
