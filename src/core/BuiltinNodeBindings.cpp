#include "talas/core/BuiltinNodeBindings.h"

#include "talas/core/BuiltinKernelRegistry.h"

#include <array>
#include <ranges>

namespace talas::core
{
bool hasBuiltinOfflineLegacyBinding(std::string_view typeId) noexcept
{
    return typeId == "talas.constant"
        || typeId == "talas.valToSound"
        || typeId == "talas.biDiFollower"
        || typeId == "talas.lossyIntegrator"
        || typeId == "talas.hysteresis"
        || typeId == "talas.oscillatorTimeIndex"
        || typeId == "talas.saturator"
        || typeId == "talas.annotation"
        || typeId == "talas.mapEventValues"
        || typeId == "talas.prefixer"
        || typeId == "talas.replicator"
        || typeId == "talas.stereoizer"
        || typeId == "talas.surroundifer"
        || typeId == "talas.sampleWithTimeIndex"
        || typeId == "talas.multiSampleWithTimeIndex"
        || typeId == "talas.splitSurroundSampleWithTimeIndex"
        || typeId == "talas.splitSurroundOscillator"
        || typeId == "talas.splitSurroundAntialiasedOscillator"
        || typeId == "talas.timeIndex"
        || typeId == "talas.timeIndexFromFileName"
        || typeId == "talas.timeIndexForSamples"
        || typeId == "talas.timeIndexForFileNames"
        || typeId == "talas.sharedFileName"
        || typeId == "talas.sharedFileNames"
        || typeId == "talas.uniqueMemoryWriterRecordingNames"
        || typeId == "talas.cancelUniqueMemoryWriterRecordingNames"
        || typeId == "talas.oscilloscopeDisplay"
        || typeId == "talas.spectrumAnalyzerDisplay"
        || typeId == "talas.feedbackLoopOutput"
        || typeId == "talas.contextFreeGrammar"
        || typeId == "talas.textFileInterpreter"
        || typeId == "talas.midiFileInterpreter"
        || typeId == "talas.cellularAutomaton"
        || typeId == "talas.rhythmicCellularAutomaton"
        || typeId == "talas.randomSelection"
        || typeId == "talas.preset"
        || typeId == "talas.diskSplicer"
        || typeId == "talas.attenuator"
        || typeId == "talas.gain"
        || typeId == "talas.centeringMixer"
        || typeId == "talas.endTogetherMixer"
        || typeId == "talas.concatenation"
        || typeId == "talas.limeInterpreter"
        || typeId == "talas.script"
        || typeId == "talas.parameterTransformer"
        || typeId == "talas.fft"
        || typeId == "talas.forwardFftV1"
        || typeId == "talas.inverseFftV1"
        || typeId == "talas.spectralShape"
        || typeId == "talas.spectrumInRam"
        || typeId == "talas.spectrumOnDisk"
        || typeId == "talas.syntheticSpectrumFromArray"
        || typeId == "talas.antialiasedOscillator"
        || typeId == "talas.harmonicSpectrum"
        || typeId == "talas.spectrumFromSingleCycle"
        || typeId == "talas.syntheticSpectrumFromSounds"
        || typeId == "talas.frameSynchronizer"
        || typeId == "talas.spectrumFrequencyScale"
        || typeId == "talas.spectrumLinearToLog"
        || typeId == "talas.spectrumLogToLinear"
        || typeId == "talas.spectrumFundamental"
        || typeId == "talas.spectrumVoicedUnvoiced"
        || typeId == "talas.spectrumModifier"
        || typeId == "talas.spectrumTrackSelector"
        || typeId == "talas.partialSpectrumTrackSelectorV1"
        || typeId == "talas.liveSpectralAnalysis"
        || typeId == "talas.partialSpectrumLegacyHoldV1"
        || typeId == "talas.pastedSoundExpressionV1"
        || typeId == "talas.partialTrackPlayerV1"
        || typeId == "talas.feedbackLoopInput"
        || typeId == "talas.audioInput"
        || typeId == "talas.genericSource"
        || typeId == "talas.diskPlayer"
        || typeId == "talas.diskCache"
        || typeId == "talas.diskRecorder"
        || typeId == "talas.diskRecorderTap"
        || typeId == "talas.memoryWriter"
        || typeId == "talas.multifileDiskPlayer"
        || typeId == "talas.midiFileEcho"
        || typeId == "talas.midiFileEchoTap"
        || typeId == "talas.midiMapper"
        || typeId == "talas.midiOutputController"
        || typeId == "talas.midiOutputEvent"
        || typeId == "talas.midiOutputControllerTap"
        || typeId == "talas.midiOutputEventTap"
        || typeId == "talas.midiOutputEventInBytes"
        || typeId == "talas.midiOutputTimingClock"
        || typeId == "talas.midiOutputStartStopMasterClock"
        || typeId == "talas.midiVoice"
        || typeId == "talas.sampleCloud"
        || typeId == "talas.cloudBank"
        || typeId == "talas.cloudBankElement"
        || typeId == "talas.sample"
        || typeId == "talas.sampleMapPlayer"
        || typeId == "talas.samplesFromDiskSingleStep"
        || typeId == "talas.multisample"
        || typeId == "talas.keyMappedMultisample"
        || typeId == "talas.soundCollectionVariable"
        || typeId == "talas.analogSequencer"
        || typeId == "talas.euSeq"
        || typeId == "talas.noise"
        || typeId == "talas.forcedProcessorAssignment"
        || typeId == "talas.analysisFilter"
        || typeId == "talas.amplitudeFollower"
        || typeId == "talas.frequencyTracker"
        || typeId == "talas.peakDetector"
        || typeId == "talas.rmsSquared"
        || typeId == "talas.morphFilter"
        || typeId == "talas.probabilityChaosGenerator"
        || typeId == "talas.ramp"
        || typeId == "talas.linearRamp"
        || typeId == "talas.sampleAndHold"
        || typeId == "talas.triggeredSampleAndHold"
        || typeId == "talas.triggeredTableRead"
        || typeId == "talas.lfo"
        || typeId == "talas.pulseTrain"
        || typeId == "talas.pulseGenerator"
        || typeId == "talas.quadOscillator"
        || typeId == "talas.quadFunctionGenerator"
        || typeId == "talas.dualMorphOscillator"
        || typeId == "talas.stereoDispersiveResonator"
        || typeId == "talas.pluckedWaveguide"
        || typeId == "talas.dynamicBreakpointOscillator"
        || typeId == "talas.waveTerrainOscillator"
        || typeId == "talas.scannedMassOscillator"
        || typeId == "talas.diffuseSpectralStretch"
        || typeId == "talas.grainCloud"
        || typeId == "talas.sampleCloud"
        || typeId == "talas.cloudBank"
        || typeId == "talas.cloudBankElement"
        || typeId == "talas.gaOscillators"
        || typeId == "talas.oscillatorBank"
        || typeId == "talas.sosOscillators"
        || typeId == "talas.sumOfSines"
        || typeId == "talas.multislopeFunctionGenerator"
        || typeId == "talas.multisegmentEnvelope"
        || typeId == "talas.graphicalEnvelope"
        || typeId == "talas.functionGenerator"
        || typeId == "talas.chopper"
        || typeId == "talas.ar"
        || typeId == "talas.adsr"
        || typeId == "talas.add"
        || typeId == "talas.difference"
        || typeId == "talas.product"
        || typeId == "talas.arcTan"
        || typeId == "talas.linearToLog"
        || typeId == "talas.logToLinear"
        || typeId == "talas.equality"
        || typeId == "talas.runningMax"
        || typeId == "talas.runningMin"
        || typeId == "talas.setRange"
        || typeId == "talas.sqrtMagnitude"
        || typeId == "talas.threshold"
        || typeId == "talas.scaleAndOffset"
        || typeId == "talas.absoluteValue"
        || typeId == "talas.interpolate"
        || typeId == "talas.oscillator"
        || typeId == "talas.mixer"
        || typeId == "talas.mixerV2"
        || typeId == "talas.cdpLowerResolution"
        || typeId == "talas.cdpRingModulate"
        || typeId == "talas.cdpTremolo"
        || typeId == "talas.cdpVaryingBank2"
        || typeId == "talas.cdpTremoloFine"
        || typeId == "talas.cdpDistmoreDouble"
        || typeId == "talas.cdpClipWaveset"
        || typeId == "talas.cdpQuirkWaveset"
        || typeId == "talas.cdpRoomReverb"
        || typeId == "talas.cdpQuantise"
        || typeId == "talas.cdpLoudnessGain"
        || typeId == "talas.cdpPhaseInvert"
        || typeId == "talas.cdpDcShift"
        || typeId == "talas.cdpCrossModulate"
        || typeId == "talas.cdpSubtract"
        || typeId == "talas.cdpMixBalance"
        || typeId == "talas.cdpMixCrossfade"
        || typeId == "talas.cdpFastConvolution"
        || typeId == "talas.cdpSilenceGate"
        || typeId == "talas.cdpDeglitch"
        || typeId == "talas.cdpNoiseCut"
        || typeId == "talas.cdpAttackBoost"
        || typeId == "talas.cdpMixMerge"
        || typeId == "talas.cdpMixFaders"
        || typeId == "talas.cdpMixInterleave"
        || typeId == "talas.cdpMixInbetween"
        || typeId == "talas.cdpEnvelopeWarp"
        || typeId == "talas.cdpEnvelopeImpose"
        || typeId == "talas.cdpEnvelopeTarget"
        || typeId == "talas.cdpPitchedDelay"
        || typeId == "talas.cdpBounce"
        || typeId == "talas.cdpRejoin"
        || typeId == "talas.cdpCascade"
        || typeId == "talas.cdpPhasor"
        || typeId == "talas.cdpSpin"
        || typeId == "talas.cdpBrassage"
        || typeId == "talas.cdpRotatingPan"
        || typeId == "talas.cdpSpreadPan"
        || typeId == "talas.cdpTablePan"
        || typeId == "talas.cdpStepSpread"
        || typeId == "talas.cdpCrossPan"
        || typeId == "talas.cdpAntiphony"
        || typeId == "talas.cdpMultichannelStadium"
        || typeId == "talas.cdpFlutter"
        || typeId == "talas.cdpFrameRotate"
        || typeId == "talas.cdpFrameRemap"
        || typeId == "talas.cdpChannelEnvelope"
        || typeId == "talas.cdpToStereo"
        || typeId == "talas.cdpStereoSpace"
        || typeId == "talas.cdpChannelZero"
        || typeId == "talas.cdpChannelMixdown"
        || typeId == "talas.cdpMonoToStereo"
        || typeId == "talas.cdpChannelPhase"
        || typeId == "talas.cdpMonoToMany"
        || typeId == "talas.cdpChannelPair"
        || typeId == "talas.cdpStereoPlacement"
        || typeId == "talas.cdpPrefix"
        || typeId == "talas.cdpSilenceMasks"
        || typeId == "talas.cdpSilenceOverwrite"
        || typeId == "talas.cdpEnvelopeTimestretch"
        || typeId == "talas.cdpPan"
        || typeId == "talas.cdpTapDelay"
        || typeId == "talas.cdpStadiumEcho"
        || typeId == "talas.cdpEcho"
        || typeId == "talas.cdpVaryingDelay"
        || typeId == "talas.cdpReverb"
        || typeId == "talas.cdpVariableFilter"
        || typeId == "talas.cdpFixedFilter"
        || typeId == "talas.cdpPhasing"
        || typeId == "talas.cdpLohiFilter"
        || typeId == "talas.cdpSweepingFilter"
        || typeId == "talas.cdpUserBank"
        || typeId == "talas.cdpDistortOverload"
        || typeId == "talas.cdpDistortReform"
        || typeId == "talas.cdpDistortOmit"
        || typeId == "talas.cdpDistortReverse"
        || typeId == "talas.cdpDistortEnvelope"
        || typeId == "talas.cdpDistortHarmonic"
        || typeId == "talas.cdpDistortFractal"
        || typeId == "talas.cdpDistortMultiply"
        || typeId == "talas.cdpDistortDivide"
        || typeId == "talas.cdpDistortShuffle"
        || typeId == "talas.cdpPhaseVocoder"
        || typeId == "talas.cdpSpectralGain"
        || typeId == "talas.cdpSpectralGate"
        || typeId == "talas.cdpFocusExaggerate"
        || typeId == "talas.cdpFocusFold"
        || typeId == "talas.cdpFocusAccumulate"
        || typeId == "talas.cdpBlurAverage"
        || typeId == "talas.cdpBlurSuppress"
        || typeId == "talas.cdpHiliteTrace"
        || typeId == "talas.cdpHiliteFilter"
        || typeId == "talas.cdpHilitePluck"
        || typeId == "talas.cdpFocusStep"
        || typeId == "talas.cdpStrangeShift"
        || typeId == "talas.cdpRepitchTranspose"
        || typeId == "talas.cdpStrangeWaver"
        || typeId == "talas.cdpPitchTransp"
        || typeId == "talas.cdpBlurChorus"
        || typeId == "talas.cdpBlurNoise"
        || typeId == "talas.cdpStretchSpectrum"
        || typeId == "talas.cdpBlurScatter"
        || typeId == "talas.cdpHiliteArpeg"
        || typeId == "talas.cdpHiliteGraphicEq"
        || typeId == "talas.cdpHiliteBand"
        || typeId == "talas.cdpBlurSpread"
        || typeId == "talas.cdpStrangeGlis"
        || typeId == "talas.cdpCombineSum"
        || typeId == "talas.cdpCombineDiff"
        || typeId == "talas.cdpCombineMax"
        || typeId == "talas.cdpCombineMean"
        || typeId == "talas.cdpCombineCross"
        || typeId == "talas.cdpFormantsVocode"
        || typeId == "talas.cdpMorphMorph"
        || typeId == "talas.cdpMorphBridge"
        || typeId == "talas.cdpRepitchTransposef"
        || typeId == "talas.cdpFormantsPut"
        || typeId == "talas.cdpHiliteVowels"
        || typeId == "talas.cdpBlurBlur"
        || typeId == "talas.cdpHiliteBlurTrace"
        || typeId == "talas.cdpFocusFocus"
        || typeId == "talas.cdpPitchTune"
        || typeId == "talas.cdpPitchPick"
        || typeId == "talas.cdpPitchChord"
        || typeId == "talas.cdpPitchChordf"
        || typeId == "talas.cdpPitchAltharms"
        || typeId == "talas.cdpPitchOctmove"
        || typeId == "talas.cdpSpecBare"
        || typeId == "talas.cdpSpecClean"
        || typeId == "talas.cdpStrangeInvert"
        || typeId == "talas.cdpFocusFreeze"
        || typeId == "talas.cdpVaryingBank"
        || typeId == "talas.overlappingMixer"
        || typeId == "talas.binauralPan"
        || typeId == "talas.pan"
        || typeId == "talas.horizontalAmbisonicEncoder"
        || typeId == "talas.horizontalAmbisonicDecoder"
        || typeId == "talas.channelCrosser"
        || typeId == "talas.channelJoin"
        || typeId == "talas.complexProduct"
        || typeId == "talas.clippingDivide"
        || typeId == "talas.minimum"
        || typeId == "talas.maximum"
        || typeId == "talas.channeller"
        || typeId == "talas.stereoMix2"
        || typeId == "talas.stereoMix4"
        || typeId == "talas.level"
        || typeId == "talas.crossfade"
        || typeId == "talas.dynamicRangeController"
        || typeId == "talas.vca"
        || typeId == "talas.filter"
        || typeId == "talas.filterBank"
        || typeId == "talas.filterBankElement"
        || typeId == "talas.formantBank"
        || typeId == "talas.formantBankOscillator"
        || typeId == "talas.firFilter"
        || typeId == "talas.highShelvingFilter"
        || typeId == "talas.lowShelvingFilter"
        || typeId == "talas.presenceFilter"
        || typeId == "talas.twoFormantElement"
        || typeId == "talas.twoFormantVoiceElement"
        || typeId == "talas.dualParallelTwoPoleFilter"
        || typeId == "talas.harmonicResonator"
        || typeId == "talas.modalFilter"
        || typeId == "talas.reResonator"
        || typeId == "talas.graphicEq"
        || typeId == "talas.vcf"
        || typeId == "talas.scaleVocoder"
        || typeId == "talas.tunableVocoder"
        || typeId == "talas.vocoder"
        || typeId == "talas.vocoderChannelBank"
        || typeId == "talas.phaseShiftBy90"
        || typeId == "talas.frequencyScale"
        || typeId == "talas.timeFrequencyScale"
        || typeId == "talas.monotonizer"
        || typeId == "talas.soundToGlobalController"
        || typeId == "talas.triggeredSoundToGlobalController"
        || typeId == "talas.simplePitchShifter"
        || typeId == "talas.variable"
        || typeId == "talas.singleSideBandRm"
        || typeId == "talas.averagingLowPassFilter"
        || typeId == "talas.lowPassGate"
        || typeId == "talas.delayWithFeedback"
        || typeId == "talas.reverbSection"
        || typeId == "talas.setDuration"
        || typeId == "talas.timeControl"
        || typeId == "talas.timeStopper"
        || typeId == "talas.timeOffset"
        || typeId == "talas.warpedTimeIndex"
        || typeId == "talas.waitUntil"
        || typeId == "talas.inputOutputCharacteristic"
        || typeId == "talas.iteratedWaveshaper"
        || typeId == "talas.multiplyingWaveshaper"
        || typeId == "talas.waveshaper"
        || typeId == "talas.matrix4"
        || typeId == "talas.matrix8"
        || typeId == "talas.output4"
        || typeId == "talas.output6"
        || typeId == "talas.output8"
        || typeId == "talas.stereoInOutput4"
        || typeId == "talas.stereoInOutput8"
        || typeId == "talas.audioOutput";
}

bool hasBuiltinPreparedRealtimeLegacyBinding(
    std::string_view typeId) noexcept
{
    static constexpr std::array typeIds = {
        std::string_view{"talas.absoluteValue"},
        std::string_view{"talas.add"},
        std::string_view{"talas.adsr"},
        std::string_view{"talas.amplitudeFollower"},
        std::string_view{"talas.analysisFilter"},
        std::string_view{"talas.analogSequencer"},
        std::string_view{"talas.euSeq"},
        std::string_view{"talas.annotation"},
        std::string_view{"talas.ar"},
        std::string_view{"talas.arcTan"},
        std::string_view{"talas.linearToLog"},
        std::string_view{"talas.logToLinear"},
        std::string_view{"talas.audioInput"},
        std::string_view{"talas.audioOutput"},
        std::string_view{"talas.averagingLowPassFilter"},
        std::string_view{"talas.biDiFollower"},
        std::string_view{"talas.lossyIntegrator"},
        std::string_view{"talas.hysteresis"},
        std::string_view{"talas.chopper"},
        std::string_view{"talas.channelCrosser"},
        std::string_view{"talas.channelJoin"},
        std::string_view{"talas.complexProduct"},
        std::string_view{"talas.clippingDivide"},
        std::string_view{"talas.minimum"},
        std::string_view{"talas.maximum"},
        std::string_view{"talas.channeller"},
        std::string_view{"talas.constant"},
        std::string_view{"talas.valToSound"},
        std::string_view{"talas.contextFreeGrammar"},
        std::string_view{"talas.textFileInterpreter"},
        std::string_view{"talas.midiFileInterpreter"},
        std::string_view{"talas.cellularAutomaton"},
        std::string_view{"talas.rhythmicCellularAutomaton"},
        std::string_view{"talas.randomSelection"},
        std::string_view{"talas.preset"},
        std::string_view{"talas.diskSplicer"},
        std::string_view{"talas.attenuator"},
        std::string_view{"talas.gain"},
        std::string_view{"talas.centeringMixer"},
        std::string_view{"talas.endTogetherMixer"},
        std::string_view{"talas.concatenation"},
        std::string_view{"talas.crossfade"},
        std::string_view{"talas.delayWithFeedback"},
        std::string_view{"talas.difference"},
        std::string_view{"talas.diskCache"},
        std::string_view{"talas.diskPlayer"},
        std::string_view{"talas.diskRecorder"},
        std::string_view{"talas.diskRecorderTap"},
        std::string_view{"talas.dualMorphOscillator"},
        std::string_view{"talas.dualParallelTwoPoleFilter"},
        std::string_view{"talas.dynamicRangeController"},
        std::string_view{"talas.equality"},
        std::string_view{"talas.fft"},
        std::string_view{"talas.forwardFftV1"},
        std::string_view{"talas.inverseFftV1"},
        std::string_view{"talas.filter"},
        std::string_view{"talas.filterBank"},
        std::string_view{"talas.filterBankElement"},
        std::string_view{"talas.formantBank"},
        std::string_view{"talas.formantBankOscillator"},
        std::string_view{"talas.feedbackLoopInput"},
        std::string_view{"talas.feedbackLoopOutput"},
        std::string_view{"talas.frequencyScale"},
        std::string_view{"talas.frequencyTracker"},
        std::string_view{"talas.firFilter"},
        std::string_view{"talas.forcedProcessorAssignment"},
        std::string_view{"talas.functionGenerator"},
        std::string_view{"talas.genericSource"},
        std::string_view{"talas.graphicalEnvelope"},
        std::string_view{"talas.graphicEq"},
        std::string_view{"talas.grainCloud"},
        std::string_view{"talas.highShelvingFilter"},
        std::string_view{"talas.harmonicResonator"},
        std::string_view{"talas.modalFilter"},
        std::string_view{"talas.sampleCloud"},
        std::string_view{"talas.cloudBank"},
        std::string_view{"talas.cloudBankElement"},
        std::string_view{"talas.inputOutputCharacteristic"},
        std::string_view{"talas.interpolate"},
        std::string_view{"talas.iteratedWaveshaper"},
        std::string_view{"talas.lfo"},
        std::string_view{"talas.limeInterpreter"},
        std::string_view{"talas.mapEventValues"},
        std::string_view{"talas.prefixer"},
        std::string_view{"talas.replicator"},
        std::string_view{"talas.stereoizer"},
        std::string_view{"talas.surroundifer"},
        std::string_view{"talas.sampleWithTimeIndex"},
        std::string_view{"talas.multiSampleWithTimeIndex"},
        std::string_view{"talas.splitSurroundSampleWithTimeIndex"},
        std::string_view{"talas.splitSurroundOscillator"},
        std::string_view{"talas.splitSurroundAntialiasedOscillator"},
        std::string_view{"talas.timeIndex"},
        std::string_view{"talas.oscillatorTimeIndex"},
        std::string_view{"talas.saturator"},
        std::string_view{"talas.timeIndexFromFileName"},
        std::string_view{"talas.timeIndexForSamples"},
        std::string_view{"talas.timeIndexForFileNames"},
        std::string_view{"talas.sharedFileName"},
        std::string_view{"talas.sharedFileNames"},
        std::string_view{"talas.uniqueMemoryWriterRecordingNames"},
        std::string_view{"talas.cancelUniqueMemoryWriterRecordingNames"},
        std::string_view{"talas.level"},
        std::string_view{"talas.linearRamp"},
        std::string_view{"talas.liveSpectralAnalysis"},
        std::string_view{"talas.lowPassGate"},
        std::string_view{"talas.lowShelvingFilter"},
        std::string_view{"talas.morphFilter"},
        std::string_view{"talas.probabilityChaosGenerator"},
        std::string_view{"talas.quadOscillator"},
        std::string_view{"talas.quadFunctionGenerator"},
        std::string_view{"talas.stereoDispersiveResonator"},
        std::string_view{"talas.pluckedWaveguide"},
        std::string_view{"talas.dynamicBreakpointOscillator"},
        std::string_view{"talas.waveTerrainOscillator"},
        std::string_view{"talas.scannedMassOscillator"},
        std::string_view{"talas.diffuseSpectralStretch"},
        std::string_view{"talas.matrix4"},
        std::string_view{"talas.matrix8"},
        std::string_view{"talas.memoryWriter"},
        std::string_view{"talas.midiFileEcho"},
        std::string_view{"talas.midiFileEchoTap"},
        std::string_view{"talas.midiMapper"},
        std::string_view{"talas.midiOutputController"},
        std::string_view{"talas.midiOutputEvent"},
        std::string_view{"talas.midiOutputControllerTap"},
        std::string_view{"talas.midiOutputEventTap"},
        std::string_view{"talas.midiOutputEventInBytes"},
        std::string_view{"talas.midiOutputTimingClock"},
        std::string_view{"talas.midiOutputStartStopMasterClock"},
        std::string_view{"talas.midiVoice"},
        std::string_view{"talas.mixer"},
        std::string_view{"talas.overlappingMixer"},
        std::string_view{"talas.monotonizer"},
        std::string_view{"talas.multiplyingWaveshaper"},
        std::string_view{"talas.multifileDiskPlayer"},
        std::string_view{"talas.multisample"},
        std::string_view{"talas.keyMappedMultisample"},
        std::string_view{"talas.multisegmentEnvelope"},
        std::string_view{"talas.multislopeFunctionGenerator"},
        std::string_view{"talas.noise"},
        std::string_view{"talas.oscillator"},
        std::string_view{"talas.gaOscillators"},
        std::string_view{"talas.oscillatorBank"},
        std::string_view{"talas.oscilloscopeDisplay"},
        std::string_view{"talas.output4"},
        std::string_view{"talas.output6"},
        std::string_view{"talas.output8"},
        std::string_view{"talas.stereoInOutput4"},
        std::string_view{"talas.stereoInOutput8"},
        std::string_view{"talas.binauralPan"},
        std::string_view{"talas.frameSynchronizer"},
        std::string_view{"talas.pan"},
        std::string_view{"talas.horizontalAmbisonicEncoder"},
        std::string_view{"talas.horizontalAmbisonicDecoder"},
        std::string_view{"talas.parameterTransformer"},
        std::string_view{"talas.partialSpectrumLegacyHoldV1"},
        std::string_view{"talas.partialSpectrumTrackSelectorV1"},
        std::string_view{"talas.pastedSoundExpressionV1"},
        std::string_view{"talas.partialTrackPlayerV1"},
        std::string_view{"talas.peakDetector"},
        std::string_view{"talas.phaseShiftBy90"},
        std::string_view{"talas.product"},
        std::string_view{"talas.presenceFilter"},
        std::string_view{"talas.pulseGenerator"},
        std::string_view{"talas.pulseTrain"},
        std::string_view{"talas.ramp"},
        std::string_view{"talas.reResonator"},
        std::string_view{"talas.reverbSection"},
        std::string_view{"talas.rmsSquared"},
        std::string_view{"talas.runningMax"},
        std::string_view{"talas.runningMin"},
        std::string_view{"talas.sample"},
        std::string_view{"talas.sampleAndHold"},
        std::string_view{"talas.sampleMapPlayer"},
        std::string_view{"talas.samplesFromDiskSingleStep"},
        std::string_view{"talas.scaleAndOffset"},
        std::string_view{"talas.script"},
        std::string_view{"talas.setDuration"},
        std::string_view{"talas.setRange"},
        std::string_view{"talas.simplePitchShifter"},
        std::string_view{"talas.singleSideBandRm"},
        std::string_view{"talas.sosOscillators"},
        std::string_view{"talas.soundCollectionVariable"},
        std::string_view{"talas.soundToGlobalController"},
        std::string_view{"talas.triggeredSoundToGlobalController"},
        std::string_view{"talas.spectralShape"},
        std::string_view{"talas.spectrumAnalyzerDisplay"},
        std::string_view{"talas.spectrumFrequencyScale"},
        std::string_view{"talas.spectrumFundamental"},
        std::string_view{"talas.spectrumInRam"},
        std::string_view{"talas.spectrumOnDisk"},
        std::string_view{"talas.spectrumLinearToLog"},
        std::string_view{"talas.spectrumLogToLinear"},
        std::string_view{"talas.spectrumModifier"},
        std::string_view{"talas.spectrumTrackSelector"},
        std::string_view{"talas.spectrumVoicedUnvoiced"},
        std::string_view{"talas.stereoMix2"},
        std::string_view{"talas.stereoMix4"},
        std::string_view{"talas.mixerV2"},
        std::string_view{"talas.cdpLowerResolution"},
        std::string_view{"talas.cdpRingModulate"},
        std::string_view{"talas.cdpTremolo"},
        std::string_view{"talas.cdpVaryingBank2"},
        std::string_view{"talas.cdpTremoloFine"},
        std::string_view{"talas.cdpDistmoreDouble"},
        std::string_view{"talas.cdpClipWaveset"},
        std::string_view{"talas.cdpQuirkWaveset"},
        std::string_view{"talas.cdpRoomReverb"},
        std::string_view{"talas.cdpQuantise"},
        std::string_view{"talas.cdpLoudnessGain"},
        std::string_view{"talas.cdpPhaseInvert"},
        std::string_view{"talas.cdpDcShift"},
        std::string_view{"talas.cdpCrossModulate"},
        std::string_view{"talas.cdpSubtract"},
        std::string_view{"talas.cdpMixBalance"},
        std::string_view{"talas.cdpMixCrossfade"},
        std::string_view{"talas.cdpFastConvolution"},
        std::string_view{"talas.cdpSilenceGate"},
        std::string_view{"talas.cdpDeglitch"},
        std::string_view{"talas.cdpNoiseCut"},
        std::string_view{"talas.cdpAttackBoost"},
        std::string_view{"talas.cdpMixMerge"},
        std::string_view{"talas.cdpMixFaders"},
        std::string_view{"talas.cdpMixInterleave"},
        std::string_view{"talas.cdpMixInbetween"},
        std::string_view{"talas.cdpEnvelopeWarp"},
        std::string_view{"talas.cdpEnvelopeImpose"},
        std::string_view{"talas.cdpEnvelopeTarget"},
        std::string_view{"talas.cdpPitchedDelay"},
        std::string_view{"talas.cdpBounce"},
        std::string_view{"talas.cdpRejoin"},
        std::string_view{"talas.cdpCascade"},
        std::string_view{"talas.cdpPhasor"},
        std::string_view{"talas.cdpSpin"},
        std::string_view{"talas.cdpBrassage"},
        std::string_view{"talas.cdpRotatingPan"},
        std::string_view{"talas.cdpSpreadPan"},
        std::string_view{"talas.cdpTablePan"},
        std::string_view{"talas.cdpStepSpread"},
        std::string_view{"talas.cdpCrossPan"},
        std::string_view{"talas.cdpAntiphony"},
        std::string_view{"talas.cdpMultichannelStadium"},
        std::string_view{"talas.cdpFlutter"},
        std::string_view{"talas.cdpFrameRotate"},
        std::string_view{"talas.cdpFrameRemap"},
        std::string_view{"talas.cdpChannelEnvelope"},
        std::string_view{"talas.cdpToStereo"},
        std::string_view{"talas.cdpStereoSpace"},
        std::string_view{"talas.cdpChannelZero"},
        std::string_view{"talas.cdpChannelMixdown"},
        std::string_view{"talas.cdpMonoToStereo"},
        std::string_view{"talas.cdpChannelPhase"},
        std::string_view{"talas.cdpMonoToMany"},
        std::string_view{"talas.cdpChannelPair"},
        std::string_view{"talas.cdpStereoPlacement"},
        std::string_view{"talas.cdpPrefix"},
        std::string_view{"talas.cdpSilenceMasks"},
        std::string_view{"talas.cdpSilenceOverwrite"},
        std::string_view{"talas.cdpEnvelopeTimestretch"},
        std::string_view{"talas.cdpPan"},
        std::string_view{"talas.cdpTapDelay"},
        std::string_view{"talas.cdpStadiumEcho"},
        std::string_view{"talas.cdpEcho"},
        std::string_view{"talas.cdpVaryingDelay"},
        std::string_view{"talas.cdpReverb"},
        std::string_view{"talas.cdpVariableFilter"},
        std::string_view{"talas.cdpFixedFilter"},
        std::string_view{"talas.cdpPhasing"},
        std::string_view{"talas.cdpLohiFilter"},
        std::string_view{"talas.cdpSweepingFilter"},
        std::string_view{"talas.cdpUserBank"},
        std::string_view{"talas.cdpDistortOverload"},
        std::string_view{"talas.cdpDistortReform"},
        std::string_view{"talas.cdpDistortOmit"},
        std::string_view{"talas.cdpDistortReverse"},
        std::string_view{"talas.cdpDistortEnvelope"},
        std::string_view{"talas.cdpDistortHarmonic"},
        std::string_view{"talas.cdpDistortFractal"},
        std::string_view{"talas.cdpDistortMultiply"},
        std::string_view{"talas.cdpDistortDivide"},
        std::string_view{"talas.cdpDistortShuffle"},
        std::string_view{"talas.cdpPhaseVocoder"},
        std::string_view{"talas.cdpSpectralGain"},
        std::string_view{"talas.cdpSpectralGate"},
        std::string_view{"talas.cdpFocusExaggerate"},
        std::string_view{"talas.cdpFocusFold"},
        std::string_view{"talas.cdpFocusAccumulate"},
        std::string_view{"talas.cdpBlurAverage"},
        std::string_view{"talas.cdpBlurSuppress"},
        std::string_view{"talas.cdpHiliteTrace"},
        std::string_view{"talas.cdpHiliteFilter"},
        std::string_view{"talas.cdpHilitePluck"},
        std::string_view{"talas.cdpFocusStep"},
        std::string_view{"talas.cdpStrangeShift"},
        std::string_view{"talas.cdpRepitchTranspose"},
        std::string_view{"talas.cdpStrangeWaver"},
        std::string_view{"talas.cdpPitchTransp"},
        std::string_view{"talas.cdpBlurChorus"},
        std::string_view{"talas.cdpBlurNoise"},
        std::string_view{"talas.cdpStretchSpectrum"},
        std::string_view{"talas.cdpBlurScatter"},
        std::string_view{"talas.cdpHiliteArpeg"},
        std::string_view{"talas.cdpHiliteGraphicEq"},
        std::string_view{"talas.cdpHiliteBand"},
        std::string_view{"talas.cdpBlurSpread"},
        std::string_view{"talas.cdpStrangeGlis"},
        std::string_view{"talas.cdpCombineSum"},
        std::string_view{"talas.cdpCombineDiff"},
        std::string_view{"talas.cdpCombineMax"},
        std::string_view{"talas.cdpCombineMean"},
        std::string_view{"talas.cdpCombineCross"},
        std::string_view{"talas.cdpFormantsVocode"},
        std::string_view{"talas.cdpMorphMorph"},
        std::string_view{"talas.cdpMorphBridge"},
        std::string_view{"talas.cdpRepitchTransposef"},
        std::string_view{"talas.cdpFormantsPut"},
        std::string_view{"talas.cdpHiliteVowels"},
        std::string_view{"talas.cdpBlurBlur"},
        std::string_view{"talas.cdpHiliteBlurTrace"},
        std::string_view{"talas.cdpFocusFocus"},
        std::string_view{"talas.cdpPitchTune"},
        std::string_view{"talas.cdpPitchPick"},
        std::string_view{"talas.cdpPitchChord"},
        std::string_view{"talas.cdpPitchChordf"},
        std::string_view{"talas.cdpPitchAltharms"},
        std::string_view{"talas.cdpPitchOctmove"},
        std::string_view{"talas.cdpSpecBare"},
        std::string_view{"talas.cdpSpecClean"},
        std::string_view{"talas.cdpStrangeInvert"},
        std::string_view{"talas.cdpFocusFreeze"},
        std::string_view{"talas.cdpVaryingBank"},
        std::string_view{"talas.sqrtMagnitude"},
        std::string_view{"talas.sumOfSines"},
        std::string_view{"talas.syntheticSpectrumFromArray"},
        std::string_view{"talas.antialiasedOscillator"},
        std::string_view{"talas.harmonicSpectrum"},
        std::string_view{"talas.spectrumFromSingleCycle"},
        std::string_view{"talas.syntheticSpectrumFromSounds"},
        std::string_view{"talas.threshold"},
        std::string_view{"talas.timeControl"},
        std::string_view{"talas.timeFrequencyScale"},
        std::string_view{"talas.timeOffset"},
        std::string_view{"talas.timeStopper"},
        std::string_view{"talas.warpedTimeIndex"},
        std::string_view{"talas.twoFormantElement"},
        std::string_view{"talas.twoFormantVoiceElement"},
        std::string_view{"talas.triggeredSampleAndHold"},
        std::string_view{"talas.triggeredTableRead"},
        std::string_view{"talas.variable"},
        std::string_view{"talas.vca"},
        std::string_view{"talas.vcf"},
        std::string_view{"talas.scaleVocoder"},
        std::string_view{"talas.tunableVocoder"},
        std::string_view{"talas.vocoder"},
        std::string_view{"talas.vocoderChannelBank"},
        std::string_view{"talas.waitUntil"},
        std::string_view{"talas.waveshaper"},
    };
    return std::ranges::find(typeIds, typeId) != typeIds.end();
}

bool hasBuiltinOfflineKernelBinding(std::string_view typeId) noexcept
{
    return findBuiltinKernelDescriptor(typeId) != nullptr;
}

bool hasBuiltinPreparedRealtimeKernelBinding(std::string_view typeId) noexcept
{
    return findBuiltinKernelDescriptor(typeId) != nullptr;
}
}
