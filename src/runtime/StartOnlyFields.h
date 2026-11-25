#pragma once

#include "CdpFixedFields.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace talas::runtime::start_only_fields
{
// The constant fields, set at compile time and not changed while the Sound
// plays: counts and sizes, seeds and
// initial states, the setup of an analysis, a gate or a voice. Given a time
// function or an Event Value, one compiles to its starting value outside a
// MIDIVoice; offline never follows one, and realtime does not refuse a time
// function in one, so a later value is neither followed nor checked.
using Field = std::pair<std::string_view, std::string_view>;

inline constexpr std::array fields{
    Field{"talas.analogSequencer", "polyphony"},
    Field{"talas.antialiasedOscillator", "dutyCycle"},
    Field{"talas.cdpLowerResolution", "sampleRateDivision"},
    Field{"talas.cellularAutomaton", "iterations"},
    Field{"talas.cellularAutomaton", "rule"},
    Field{"talas.cloudBank", "bankSize"},
    Field{"talas.cloudBank", "grainsPerTrack"},
    Field{"talas.cloudBank", "nbrTracks"},
    Field{"talas.cloudBank", "seed"},
    Field{"talas.cloudBankElement", "count"},
    Field{"talas.cloudBankElement", "first"},
    Field{"talas.cloudBankElement", "initialDelay"},
    Field{"talas.cloudBankElement", "maxGrains"},
    Field{"talas.cloudBankElement", "seed"},
    Field{"talas.contextFreeGrammar", "left"},
    Field{"talas.contextFreeGrammar", "maxRewrites"},
    Field{"talas.contextFreeGrammar", "maxSize"},
    Field{"talas.contextFreeGrammar", "right"},
    Field{"talas.contextFreeGrammar", "seed"},
    Field{"talas.diffuseSpectralStretch", "diffusion"},
    Field{"talas.diffuseSpectralStretch", "seed"},
    Field{"talas.diffuseSpectralStretch", "smear"},
    Field{"talas.diffuseSpectralStretch", "stretch"},
    Field{"talas.dynamicBreakpointOscillator", "pointCount"},
    Field{"talas.dynamicBreakpointOscillator", "seed"},
    Field{"talas.euSeq", "pulses"},
    Field{"talas.euSeq", "scaleSteps"},
    Field{"talas.euSeq", "steps"},
    Field{"talas.euSeq", "stepsPerOctave"},
    Field{"talas.feedbackLoopOutput", "delay"},
    Field{"talas.filter", "order"},
    Field{"talas.filterBank", "bankSize"},
    Field{"talas.filterBank", "nbrFilters"},
    Field{"talas.filterBankElement", "count"},
    Field{"talas.filterBankElement", "first"},
    Field{"talas.forcedProcessorAssignment", "processor"},
    Field{"talas.formantBank", "bankSize"},
    Field{"talas.formantBank", "nbrFormants"},
    Field{"talas.formantBank", "nbrImpulses"},
    Field{"talas.formantBankOscillator", "firstFormant"},
    Field{"talas.formantBankOscillator", "nbrFormants"},
    Field{"talas.formantBankOscillator", "nbrImpulses"},
    Field{"talas.frameSynchronizer", "frameLength"},
    Field{"talas.frequencyScale", "maxScale"},
    Field{"talas.frequencyTracker", "detectors"},
    Field{"talas.grainCloud", "maxGrains"},
    Field{"talas.harmonicSpectrum", "amplitude"},
    Field{"talas.harmonicSpectrum", "frequency"},
    Field{"talas.harmonicSpectrum", "nbrPartials"},
    Field{"talas.liveSpectralAnalysis", "highFreq"},
    Field{"talas.liveSpectralAnalysis", "initFreq"},
    Field{"talas.liveSpectralAnalysis", "lowFreq"},
    Field{"talas.liveSpectralAnalysis", "trackedHarmonic"},
    Field{"talas.liveSpectralAnalysis", "unpitchedOnly"},
    Field{"talas.liveSpectralAnalysis", "unpitchedThreshold"},
    Field{"talas.lowPassGate", "attackTime"},
    Field{"talas.lowPassGate", "closedFrequency"},
    Field{"talas.lowPassGate", "curve"},
    Field{"talas.lowPassGate", "openFrequency"},
    Field{"talas.lowPassGate", "releaseTime"},
    Field{"talas.lowPassGate", "scale"},
    Field{"talas.midiMapper", "channel"},
    Field{"talas.midiMapper", "highPitch"},
    Field{"talas.midiMapper", "lowPitch"},
    Field{"talas.midiMapper", "polyphony"},
    Field{"talas.midiVoice", "channel"},
    Field{"talas.midiVoice", "highPitch"},
    Field{"talas.midiVoice", "lowPitch"},
    Field{"talas.midiVoice", "polyphony"},
    Field{"talas.multisegmentEnvelope", "endLoop"},
    Field{"talas.multisegmentEnvelope", "startLoop"},
    Field{"talas.noise", "initialState"},
    Field{"talas.oscillator", "maxMI"},
    Field{"talas.oscillatorBank", "bankSize"},
    Field{"talas.oscillatorBank", "nbrOscillators"},
    Field{"talas.oscilloscopeDisplay", "initialSweepFrequency"},
    Field{"talas.oscilloscopeDisplay", "level"},
    Field{"talas.oscilloscopeDisplay", "sweepFreq"},
    Field{"talas.overlappingMixer", "overlapTime"},
    Field{"talas.partialSpectrumTrackSelectorV1", "track"},
    Field{"talas.pluckedWaveguide", "seed"},
    Field{"talas.probabilityChaosGenerator", "seed"},
    Field{"talas.replicator", "number"},
    Field{"talas.rhythmicCellularAutomaton", "generationLength"},
    Field{"talas.rhythmicCellularAutomaton", "generations"},
    Field{"talas.rhythmicCellularAutomaton", "rule"},
    Field{"talas.sampleCloud", "maxGrains"},
    Field{"talas.scaleVocoder", "bankSize"},
    Field{"talas.scaleVocoder", "nbrBands"},
    Field{"talas.scannedMassOscillator", "initialShape"},
    Field{"talas.scannedMassOscillator", "massCount"},
    Field{"talas.simplePitchShifter", "maxInputPitch"},
    Field{"talas.simplePitchShifter", "minInputPitch"},
    Field{"talas.sosOscillators", "firstOscillator"},
    Field{"talas.sosOscillators", "nbrOscillators"},
    Field{"talas.spectralShape", "nbrPartials"},
    Field{"talas.spectrumFromSingleCycle", "frequency"},
    Field{"talas.spectrumInRam", "analysisFrames"},
    Field{"talas.spectrumInRam", "analysisFrequency"},
    Field{"talas.spectrumInRam", "firstPartial"},
    Field{"talas.spectrumInRam", "nbrPartials"},
    Field{"talas.spectrumModifier", "seed"},
    Field{"talas.spectrumOnDisk", "analysisFrames"},
    Field{"talas.spectrumOnDisk", "analysisFrequency"},
    Field{"talas.spectrumOnDisk", "firstPartial"},
    Field{"talas.spectrumOnDisk", "nbrPartials"},
    Field{"talas.spectrumTrackSelector", "track"},
    Field{"talas.sumOfSines", "analysisFrames0"},
    Field{"talas.sumOfSines", "analysisFrames1"},
    Field{"talas.sumOfSines", "analysisFrequency0"},
    Field{"talas.sumOfSines", "analysisFrequency1"},
    Field{"talas.syntheticSpectrumFromArray", "nbrPartials"},
    Field{"talas.syntheticSpectrumFromSounds", "nbrPartials"},
    Field{"talas.timeFrequencyScale", "detectors"},
    Field{"talas.timeFrequencyScale", "maxFreqScale"},
    Field{"talas.timeFrequencyScale", "maxInputFreq"},
    Field{"talas.timeFrequencyScale", "minInputFreq"},
    Field{"talas.tunableVocoder", "bankSize"},
    Field{"talas.tunableVocoder", "nbrBands"},
    Field{"talas.vocoder", "bankSize"},
    Field{"talas.vocoder", "nbrBands"},
    Field{"talas.vocoderChannelBank", "count"},
    Field{"talas.vocoderChannelBank", "first"},
    Field{"talas.waveTerrainOscillator", "height"},
    Field{"talas.waveTerrainOscillator", "width"},
};

[[nodiscard]] constexpr bool contains(std::string_view typeId, std::string_view parameterId) noexcept
{
    return std::ranges::find(fields, Field{typeId, parameterId}) != fields.end()
        || std::ranges::find(cdpFixedFields, Field{typeId, parameterId}) != cdpFixedFields.end();
}

// Owner decision 2026-10-12: inside a MIDIVoice a constant field is read as
// each voice starts, with that voice's Event Values. These fields fix what a
// voice cannot set up again as it starts: the Sound's latency (a
// FrequencyScale's MaxScale; a TimeFrequencyScale's MinInputFreq, which sizes
// its analysis window, and MaxFreqScale, which decides whether a fixed
// FrequencyScale uses the vocoder at all; CDP's division), a structure (a
// ContextFreeGrammar's expansion, a Replicator's copies, a cellular
// automaton's rule and generations, a feedback loop's shared delay, a processor
// placement, an oscilloscope, an OverlappingMixer's start times), or a
// count our renderers hold fixed (a
// TimeFrequencyScale's and FrequencyTracker's ten detectors, the only count
// either renderer plays), or the size of a bank or a spectrum and the tracks
// it reads (a bank's oscillators, filters, formants, grains and impulses, a
// spectrum's partials and analysis, a selected track, a terrain or a frame),
// with the seeds, delay, and the harmonic tracker's estimate, range and
// threshold that set such a bank or analysis going (taken as recommended,
// 2026-10-13), and the fundamental and amplitude a HarmonicSpectrum or
// SpectrumFromSingleCycle computes its static spectrum from (our extensions,
// which nothing documents as hot; 2026-10-13). Inside a MIDIVoice, one
// that follows an Event Value or time is
// refused by name. A vocoder's NbrBands, a SumOfSines's analysis frames and
// an AntialiasedOscillator's DutyCycle are read per voice (2026-10-13): the
// bank holds room for 64 bands, the analyses are kept whole, and the cycle's
// transform is planned when the voice is prepared.
inline constexpr std::array fixedWhenCompiled{
    Field{"talas.cdpLowerResolution", "sampleRateDivision"},
    Field{"talas.cellularAutomaton", "iterations"},
    Field{"talas.cellularAutomaton", "rule"},
    Field{"talas.cloudBank", "bankSize"},
    Field{"talas.cloudBank", "grainsPerTrack"},
    Field{"talas.cloudBank", "nbrTracks"},
    Field{"talas.cloudBank", "seed"},
    Field{"talas.cloudBankElement", "count"},
    Field{"talas.cloudBankElement", "first"},
    Field{"talas.cloudBankElement", "initialDelay"},
    Field{"talas.cloudBankElement", "maxGrains"},
    Field{"talas.cloudBankElement", "seed"},
    Field{"talas.contextFreeGrammar", "left"},
    Field{"talas.contextFreeGrammar", "maxRewrites"},
    Field{"talas.contextFreeGrammar", "maxSize"},
    Field{"talas.contextFreeGrammar", "right"},
    Field{"talas.contextFreeGrammar", "seed"},
    Field{"talas.feedbackLoopOutput", "delay"},
    Field{"talas.filterBank", "bankSize"},
    Field{"talas.filterBank", "nbrFilters"},
    Field{"talas.filterBankElement", "count"},
    Field{"talas.filterBankElement", "first"},
    Field{"talas.forcedProcessorAssignment", "processor"},
    Field{"talas.formantBank", "bankSize"},
    Field{"talas.formantBank", "nbrFormants"},
    Field{"talas.formantBank", "nbrImpulses"},
    Field{"talas.formantBankOscillator", "firstFormant"},
    Field{"talas.formantBankOscillator", "nbrFormants"},
    Field{"talas.formantBankOscillator", "nbrImpulses"},
    Field{"talas.frameSynchronizer", "frameLength"},
    Field{"talas.frequencyScale", "maxScale"},
    Field{"talas.frequencyTracker", "detectors"},
    Field{"talas.harmonicSpectrum", "amplitude"},
    Field{"talas.harmonicSpectrum", "frequency"},
    Field{"talas.harmonicSpectrum", "nbrPartials"},
    Field{"talas.liveSpectralAnalysis", "highFreq"},
    Field{"talas.liveSpectralAnalysis", "initFreq"},
    Field{"talas.liveSpectralAnalysis", "lowFreq"},
    Field{"talas.liveSpectralAnalysis", "trackedHarmonic"},
    Field{"talas.liveSpectralAnalysis", "unpitchedOnly"},
    Field{"talas.liveSpectralAnalysis", "unpitchedThreshold"},
    Field{"talas.oscillatorBank", "bankSize"},
    Field{"talas.oscillatorBank", "nbrOscillators"},
    Field{"talas.oscilloscopeDisplay", "initialSweepFrequency"},
    Field{"talas.oscilloscopeDisplay", "level"},
    Field{"talas.oscilloscopeDisplay", "sweepFreq"},
    Field{"talas.overlappingMixer", "overlapTime"},
    Field{"talas.partialSpectrumTrackSelectorV1", "track"},
    Field{"talas.replicator", "number"},
    Field{"talas.rhythmicCellularAutomaton", "generationLength"},
    Field{"talas.rhythmicCellularAutomaton", "generations"},
    Field{"talas.rhythmicCellularAutomaton", "rule"},
    Field{"talas.sosOscillators", "firstOscillator"},
    Field{"talas.sosOscillators", "nbrOscillators"},
    Field{"talas.spectralShape", "nbrPartials"},
    Field{"talas.spectrumFromSingleCycle", "frequency"},
    Field{"talas.spectrumInRam", "analysisFrames"},
    Field{"talas.spectrumInRam", "analysisFrequency"},
    Field{"talas.spectrumInRam", "firstPartial"},
    Field{"talas.spectrumInRam", "nbrPartials"},
    Field{"talas.spectrumModifier", "seed"},
    Field{"talas.spectrumOnDisk", "analysisFrames"},
    Field{"talas.spectrumOnDisk", "analysisFrequency"},
    Field{"talas.spectrumOnDisk", "firstPartial"},
    Field{"talas.spectrumOnDisk", "nbrPartials"},
    Field{"talas.spectrumTrackSelector", "track"},
    Field{"talas.syntheticSpectrumFromSounds", "nbrPartials"},
    Field{"talas.timeFrequencyScale", "detectors"},
    Field{"talas.timeFrequencyScale", "maxFreqScale"},
    Field{"talas.timeFrequencyScale", "minInputFreq"},
    Field{"talas.vocoderChannelBank", "count"},
    Field{"talas.vocoderChannelBank", "first"},
    Field{"talas.waveTerrainOscillator", "height"},
    Field{"talas.waveTerrainOscillator", "width"},
};

[[nodiscard]] constexpr bool fixedWhenCompiledContains(std::string_view typeId, std::string_view parameterId) noexcept
{
    // A CDP fixed field sets up the process when it starts; a voice cannot
    // set it up again, so inside a MIDIVoice a moving one is named.
    return std::ranges::find(fixedWhenCompiled, Field{typeId, parameterId}) != fixedWhenCompiled.end()
        || std::ranges::find(cdpFixedFields, Field{typeId, parameterId}) != cdpFixedFields.end();
}
}
