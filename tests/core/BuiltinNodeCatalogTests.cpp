#include "talas/core/BuiltinNodes.h"
#include "talas/core/Graph.h"
#include "talas/core/AnalogSequencerExtraValues.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectSoundVariables.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireUniqueIds(const std::vector<talas::core::PortDescriptor>& ports, const std::string& nodeTypeId, const std::string& direction)
{
    std::unordered_set<std::string> ids;
    for (const auto& port : ports)
        require(ids.insert(port.id).second, nodeTypeId + " should not duplicate " + direction + " port id: " + port.id);
}

void requireUniqueIds(const std::vector<talas::core::ParameterDescriptor>& parameters, const std::string& nodeTypeId)
{
    std::unordered_set<std::string> ids;
    for (const auto& parameter : parameters)
        require(ids.insert(parameter.id).second, nodeTypeId + " should not duplicate parameter id: " + parameter.id);
}

bool isAsciiLower(char value)
{
    return value >= 'a' && value <= 'z';
}

bool isAsciiUpper(char value)
{
    return value >= 'A' && value <= 'Z';
}

bool isAsciiDigit(char value)
{
    return value >= '0' && value <= '9';
}

bool isLowerCamelIdentifier(const std::string& id)
{
    if (id.empty() || !isAsciiLower(id.front()))
        return false;

    return std::ranges::all_of(id, [](char value) {
        return isAsciiLower(value) || isAsciiUpper(value) || isAsciiDigit(value);
    });
}

const talas::core::NodeTypeDescriptor& requireNodeType(const talas::core::NodeRegistry& registry, const std::string& typeId)
{
    const auto* descriptor = registry.findNodeType(typeId);
    require(descriptor != nullptr, "built-in registry should contain " + typeId);
    return *descriptor;
}

const talas::core::ParameterDescriptor& requireParameter(
    const talas::core::NodeRegistry& registry,
    const std::string& nodeTypeId,
    const std::string& parameterId)
{
    const auto& descriptor = requireNodeType(registry, nodeTypeId);
    const auto parameter = std::ranges::find_if(descriptor.parameters, [&](const auto& candidate) {
        return candidate.id == parameterId;
    });

    require(parameter != descriptor.parameters.end(), nodeTypeId + " should contain parameter " + parameterId);
    return *parameter;
}

struct ExpectedPort
{
    std::string id;
    talas::core::PortType type;
    bool required;
};

struct ExpectedParameter
{
    std::string id;
    std::string defaultExpression;
    talas::expr::ValueDomain domain;
    std::string defaultChoice;
    std::vector<std::string> choices;
    std::optional<bool> defaultBoolean;
    std::string defaultResource;
    std::string defaultText;
    std::optional<talas::core::ParameterKind> kind;
};

struct ExpectedNodeDescriptor
{
    std::string typeId;
    std::string displayName;
    std::string category;
    std::vector<ExpectedPort> inputs;
    std::vector<ExpectedPort> outputs;
    std::vector<ExpectedParameter> parameters;
};

void requirePorts(
    const std::vector<talas::core::PortDescriptor>& actual,
    const std::vector<ExpectedPort>& expected,
    const std::string& nodeTypeId,
    const std::string& direction)
{
    std::vector<const talas::core::PortDescriptor*> presented;
    for (const auto& port : actual)
    {
        if (!port.compilerPopulated)
            presented.push_back(&port);
    }
    require(presented.size() == expected.size(), nodeTypeId + " should declare exact " + direction + " port count");
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(presented[index]->id == expected[index].id, nodeTypeId + " should declare " + direction + " port id " + expected[index].id);
        require(presented[index]->type == expected[index].type, nodeTypeId + " should declare expected " + direction + " port type for " + expected[index].id);
        require(presented[index]->required == expected[index].required, nodeTypeId + " should declare expected " + direction + " port required flag for " + expected[index].id);
    }
}

void requireParameters(
    const std::vector<talas::core::ParameterDescriptor>& actual,
    const std::vector<ExpectedParameter>& expected,
    const std::string& nodeTypeId)
{
    require(actual.size() == expected.size(), nodeTypeId + " should declare exact parameter count");
    for (std::size_t index = 0; index < expected.size(); ++index)
    {
        require(actual[index].id == expected[index].id, nodeTypeId + " should declare parameter id " + expected[index].id);
        require(actual[index].defaultExpression == expected[index].defaultExpression, nodeTypeId + " should declare expected default expression for " + expected[index].id);
        require(actual[index].domain == expected[index].domain, nodeTypeId + " should declare expected parameter domain for " + expected[index].id);
        require(actual[index].defaultChoice == expected[index].defaultChoice, nodeTypeId + " should declare expected default choice for " + expected[index].id);
        require(actual[index].choices == expected[index].choices, nodeTypeId + " should declare expected choices for " + expected[index].id);
        require(actual[index].defaultBoolean == expected[index].defaultBoolean, nodeTypeId + " should declare expected boolean default for " + expected[index].id);
        require(actual[index].defaultResource == expected[index].defaultResource, nodeTypeId + " should declare expected resource default for " + expected[index].id);
        require(actual[index].defaultText == expected[index].defaultText, nodeTypeId + " should declare expected text default for " + expected[index].id);
        const auto inferredKind = expected[index].defaultBoolean.has_value()
            ? talas::core::ParameterKind::Boolean
            : expected[index].choices.empty()
            ? talas::core::ParameterKind::Expression
            : talas::core::ParameterKind::Choice;
        const auto expectedKind = expected[index].kind.value_or(inferredKind);
        require(actual[index].kind == expectedKind, nodeTypeId + " should declare expected parameter kind for " + expected[index].id);
    }
}

std::vector<std::string> expectedBuiltinTypeIds()
{
    return {
        "talas.oscillator",
        "talas.antialiasedOscillator",
        "talas.noise",
        "talas.sample",
        "talas.sampleMapPlayer",
        "talas.forcedProcessorAssignment",
        "talas.diskCache",
        "talas.diskPlayer",
        "talas.partialTrackPlayerV1",
        "talas.diskRecorder",
        "talas.diskRecorderTap",
        "talas.memoryWriter",
        "talas.crossfade",
        "talas.dynamicRangeController",
        "talas.keyMappedMultisample",
        "talas.level",
        "talas.multifileDiskPlayer",
        "talas.multiplyingWaveshaper",
        "talas.multisample",
        "talas.sampleAndHold",
        "talas.sampleCloud",
        "talas.samplesFromDiskSingleStep",
        "talas.frequencyScale",
        "talas.monotonizer",
        "talas.simplePitchShifter",
        "talas.singleSideBandRm",
        "talas.spectralShape",
        "talas.reverbSection",
        "talas.spectrumFrequencyScale",
        "talas.spectrumFundamental",
        "talas.spectrumInRam",
        "talas.spectrumLinearToLog",
        "talas.spectrumLogToLinear",
        "talas.spectrumModifier",
        "talas.spectrumOnDisk",
        "talas.spectrumTrackSelector",
        "talas.partialSpectrumTrackSelectorV1",
        "talas.spectrumVoicedUnvoiced",
        "talas.soundCollectionVariable",
        "talas.spectrumFromSingleCycle",
        "talas.harmonicSpectrum",
        "talas.syntheticSpectrumFromArray",
        "talas.syntheticSpectrumFromSounds",
        "talas.frameSynchronizer",
        "talas.uniqueMemoryWriterRecordingNames",
        "talas.cancelUniqueMemoryWriterRecordingNames",
        "talas.feedbackLoopInput",
        "talas.feedbackLoopOutput",
        "talas.fft",
        "talas.forwardFftV1",
        "talas.inverseFftV1",
        "talas.timeFrequencyScale",
        "talas.contextFreeGrammar",
        "talas.cellularAutomaton",
        "talas.midiFileInterpreter",
        "talas.preset",
        "talas.diskSplicer",
        "talas.attenuator",
        "talas.gain",
        "talas.centeringMixer",
        "talas.endTogetherMixer",
        "talas.concatenation",
        "talas.randomSelection",
        "talas.rhythmicCellularAutomaton",
        "talas.textFileInterpreter",
        "talas.limeInterpreter",
        "talas.parameterTransformer",
        "talas.partialSpectrumLegacyHoldV1",
        "talas.pastedSoundExpressionV1",
        "talas.script",
        "talas.analogSequencer",
        "talas.euSeq",
        "talas.constant",
        "talas.valToSound",
        "talas.functionGenerator",
        "talas.audioInput",
        "talas.amplitudeFollower",
        "talas.biDiFollower",
        "talas.lossyIntegrator",
        "talas.hysteresis",
        "talas.analysisFilter",
        "talas.adsr",
        "talas.ar",
        "talas.chopper",
        "talas.cloudBank",
        "talas.cloudBankElement",
        "talas.linearRamp",
        "talas.ramp",
        "talas.lfo",
        "talas.clock",
        "talas.clockDivider",
        "talas.quantizer",
        "talas.gate",
        "talas.logic",
        "talas.slew",
        "talas.frequencyTracker",
        "talas.graphicalEnvelope",
        "talas.liveSpectralAnalysis",
        "talas.horizontalAmbisonicEncoder",
        "talas.horizontalAmbisonicDecoder",
        "talas.matrix4",
        "talas.matrix8",
        "talas.midiFileEcho",
        "talas.midiFileEchoTap",
        "talas.midiMapper",
        "talas.midiOutputController",
        "talas.midiOutputControllerTap",
        "talas.midiOutputEvent",
        "talas.midiOutputEventTap",
        "talas.midiOutputEventInBytes",
        "talas.midiOutputTimingClock",
        "talas.midiOutputStartStopMasterClock",
        "talas.midiVoice",
        "talas.multisegmentEnvelope",
        "talas.multislopeFunctionGenerator",
        "talas.peakDetector",
        "talas.rmsSquared",
        "talas.soundToGlobalController",
        "talas.triggeredSoundToGlobalController",
        "talas.sosOscillators",
        "talas.threshold",
        "talas.stereoInOutput4",
        "talas.stereoInOutput8",
        "talas.add",
        "talas.difference",
        "talas.product",
        "talas.arcTan",
        "talas.linearToLog",
        "talas.logToLinear",
        "talas.equality",
        "talas.runningMax",
        "talas.runningMin",
        "talas.phaseShiftBy90",
        "talas.setRange",
        "talas.sqrtMagnitude",
        "talas.sumOfSines",
        "talas.scaleAndOffset",
        "talas.absoluteValue",
        "talas.interpolate",
        "talas.channelCrosser",
        "talas.channelJoin",
        "talas.complexProduct",
        "talas.clippingDivide",
        "talas.mapEventValues",
        "talas.prefixer",
        "talas.replicator",
        "talas.stereoizer",
        "talas.surroundifer",
        "talas.sampleWithTimeIndex",
        "talas.multiSampleWithTimeIndex",
        "talas.splitSurroundSampleWithTimeIndex",
        "talas.splitSurroundOscillator",
        "talas.splitSurroundAntialiasedOscillator",
        "talas.oscillatorTimeIndex",
        "talas.saturator",
        "talas.timeIndex",
        "talas.timeIndexFromFileName",
        "talas.timeIndexForSamples",
        "talas.timeIndexForFileNames",
        "talas.sharedFileName",
        "talas.sharedFileNames",
        "talas.minimum",
        "talas.maximum",
        "talas.channeller",
        "talas.mixer",
        "talas.mixerV2",
        "talas.cdpLowerResolution",
        "talas.cdpRingModulate",
        "talas.cdpTremolo",
        "talas.cdpPan",
        "talas.cdpTapDelay",
        "talas.cdpEcho",
        "talas.cdpVaryingDelay",
        "talas.cdpReverb",
        "talas.cdpVariableFilter",
        "talas.cdpFixedFilter",
        "talas.cdpPhasing",
        "talas.cdpLohiFilter",
        "talas.cdpSweepingFilter",
        "talas.cdpUserBank",
        "talas.cdpDistortOverload",
        "talas.cdpVaryingBank",
        "talas.cdpStadiumEcho",
        "talas.cdpDistortReform",
        "talas.cdpDistortOmit",
        "talas.cdpDistortReverse",
        "talas.cdpDistortEnvelope",
        "talas.cdpDistortHarmonic",
        "talas.cdpDistortFractal",
        "talas.cdpDistortMultiply",
        "talas.cdpDistortDivide",
        "talas.cdpDistortShuffle",
        "talas.cdpPhaseVocoder",
        "talas.cdpSpectralGain",
        "talas.cdpSpectralGate",
        "talas.cdpBlurAverage",
        "talas.cdpBlurSuppress",
        "talas.cdpFocusAccumulate",
        "talas.cdpFocusExaggerate",
        "talas.cdpFocusFold",
        "talas.cdpHiliteTrace",
        "talas.cdpFocusStep",
        "talas.cdpHiliteFilter",
        "talas.cdpHilitePluck",
        "talas.cdpRepitchTranspose",
        "talas.cdpStrangeShift",
        "talas.cdpStrangeWaver",
        "talas.cdpPitchTransp",
        "talas.cdpBlurChorus",
        "talas.cdpBlurNoise",
        "talas.cdpStretchSpectrum",
        "talas.cdpBlurScatter",
        "talas.cdpHiliteArpeg",
        "talas.cdpHiliteBand",
        "talas.cdpHiliteGraphicEq",
        "talas.cdpBlurSpread",
        "talas.cdpStrangeGlis",
        "talas.cdpCombineCross",
        "talas.cdpCombineDiff",
        "talas.cdpCombineMax",
        "talas.cdpCombineMean",
        "talas.cdpCombineSum",
        "talas.cdpFormantsVocode",
        "talas.cdpMorphBridge",
        "talas.cdpMorphMorph",
        "talas.cdpRepitchTransposef",
        "talas.cdpFormantsPut",
        "talas.cdpHiliteVowels",
        "talas.cdpBlurBlur",
        "talas.cdpHiliteBlurTrace",
        "talas.cdpFocusFocus",
        "talas.cdpPitchChord",
        "talas.cdpPitchChordf",
        "talas.cdpPitchPick",
        "talas.cdpPitchTune",
        "talas.cdpFocusFreeze",
        "talas.cdpPitchAltharms",
        "talas.cdpPitchOctmove",
        "talas.cdpSpecBare",
        "talas.cdpSpecClean",
        "talas.cdpStrangeInvert",
        "talas.cdpVaryingBank2",
        "talas.cdpTremoloFine",
        "talas.cdpDistmoreDouble",
        "talas.cdpClipWaveset",
        "talas.cdpQuirkWaveset",
        "talas.cdpRoomReverb",
        "talas.cdpQuantise",
        "talas.cdpLoudnessGain",
        "talas.cdpPhaseInvert",
        "talas.cdpDcShift",
        "talas.cdpCrossModulate",
        "talas.cdpSubtract",
        "talas.cdpMixBalance",
        "talas.cdpMixCrossfade",
        "talas.cdpFastConvolution",
        "talas.cdpSilenceGate",
        "talas.cdpDeglitch",
        "talas.cdpNoiseCut",
        "talas.cdpAttackBoost",
        "talas.cdpMixMerge",
        "talas.cdpMixFaders",
        "talas.cdpMixInterleave",
        "talas.cdpMixInbetween",
        "talas.cdpEnvelopeWarp",
        "talas.cdpEnvelopeImpose",
        "talas.cdpEnvelopeTarget",
        "talas.cdpPitchedDelay",
        "talas.cdpBounce",
        "talas.cdpRejoin",
        "talas.cdpCascade",
        "talas.cdpPhasor",
        "talas.cdpSpin",
        "talas.cdpBrassage",
        "talas.cdpRotatingPan",
        "talas.cdpSpreadPan",
        "talas.cdpTablePan",
        "talas.cdpStepSpread",
        "talas.cdpCrossPan",
        "talas.cdpAntiphony",
        "talas.cdpMultichannelStadium",
        "talas.cdpFlutter",
        "talas.cdpFrameRotate",
        "talas.cdpFrameRemap",
        "talas.cdpChannelEnvelope",
        "talas.cdpToStereo",
        "talas.cdpStereoSpace",
        "talas.cdpChannelZero",
        "talas.cdpChannelMixdown",
        "talas.cdpMonoToStereo",
        "talas.cdpChannelPhase",
        "talas.cdpMonoToMany",
        "talas.cdpChannelPair",
        "talas.cdpStereoPlacement",
        "talas.cdpPrefix",
        "talas.cdpSilenceMasks",
        "talas.cdpSilenceOverwrite",
        "talas.cdpEnvelopeTimestretch",
        "talas.overlappingMixer",
        "talas.output4",
        "talas.output6",
        "talas.output8",
        "talas.binauralPan",
        "talas.pan",
        "talas.pulseGenerator",
        "talas.pulseTrain",
        "talas.quadOscillator",
        "talas.quadFunctionGenerator",
        "talas.dualMorphOscillator",
        "talas.probabilityChaosGenerator",
        "talas.stereoMix2",
        "talas.stereoMix4",
        "talas.vca",
        "talas.filter",
        "talas.averagingLowPassFilter",
        "talas.lowPassGate",
        "talas.morphFilter",
        "talas.dualParallelTwoPoleFilter",
        "talas.firFilter",
        "talas.filterBank",
        "talas.filterBankElement",
        "talas.formantBank",
        "talas.formantBankOscillator",
        "talas.gaOscillators",
        "talas.genericSource",
        "talas.graphicEq",
        "talas.grainCloud",
        "talas.harmonicResonator",
        "talas.modalFilter",
        "talas.highShelvingFilter",
        "talas.lowShelvingFilter",
        "talas.presenceFilter",
        "talas.reResonator",
        "talas.scaleVocoder",
        "talas.tunableVocoder",
        "talas.twoFormantElement",
        "talas.vcf",
        "talas.vocoder",
        "talas.vocoderChannelBank",
        "talas.delayWithFeedback",
        "talas.stereoDispersiveResonator",
        "talas.pluckedWaveguide",
        "talas.dynamicBreakpointOscillator",
        "talas.waveTerrainOscillator",
        "talas.scannedMassOscillator",
        "talas.diffuseSpectralStretch",
        "talas.triggeredTableRead",
        "talas.triggeredSampleAndHold",
        "talas.inputOutputCharacteristic",
        "talas.iteratedWaveshaper",
        "talas.waveshaper",
        "talas.setDuration",
        "talas.timeControl",
        "talas.timeStopper",
        "talas.timeOffset",
        "talas.twoFormantVoiceElement",
        "talas.waitUntil",
        "talas.warpedTimeIndex",
        "talas.audioOutput",
        "talas.annotation",
        "talas.variable",
        "talas.oscilloscopeDisplay",
        "talas.oscillatorBank",
        "talas.spectrumAnalyzerDisplay",
    };
}

void testBuiltinRegistryContainsEveryV1NodeType()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto descriptors = registry.nodeTypes();
    const auto expectedIds = expectedBuiltinTypeIds();

    std::unordered_set<std::string> actualIds;
    for (const auto& descriptor : descriptors)
        actualIds.insert(descriptor.typeId);

    require(descriptors.size() == expectedIds.size(), "built-in registry should expose exactly the declared descriptor set");
    for (const auto& typeId : expectedIds)
        require(actualIds.contains(typeId), "built-in registry should contain " + typeId);
}

#if defined(__APPLE__)
void* buildBuiltinRegistryOnWorkerStack(void* context) noexcept
{
    auto& success = *static_cast<bool*>(context);
    try
    {
        const auto registry = talas::core::makeBuiltinNodeRegistry();
        success = registry.findNodeType("talas.oscillator") != nullptr
            && registry.findNodeType("talas.analogSequencer") != nullptr;
    }
    catch (...)
    {
        success = false;
    }
    return nullptr;
}

void testBuiltinRegistryFitsWorkbenchWorkerStack()
{
    pthread_attr_t attributes{};
    require(pthread_attr_init(&attributes) == 0,
        "builtin registry worker attributes should initialize");
    const auto stackStatus = pthread_attr_setstacksize(
        &attributes, 512 * 1024);
    if (stackStatus != 0)
    {
        pthread_attr_destroy(&attributes);
        require(false, "builtin registry worker stack should be bounded");
    }
    bool success = false;
    pthread_t thread{};
    const auto createStatus = pthread_create(
        &thread, &attributes, buildBuiltinRegistryOnWorkerStack, &success);
    pthread_attr_destroy(&attributes);
    require(createStatus == 0,
        "builtin registry should start on a Workbench-sized worker stack");
    require(pthread_join(thread, nullptr) == 0 && success,
        "builtin registry should finish on a Workbench-sized worker stack");
}
#endif

void testBuiltinDescriptorIdsAreUniquePerNode()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();

    for (const auto& descriptor : registry.nodeTypes())
    {
        requireUniqueIds(descriptor.inputs, descriptor.typeId, "input");
        requireUniqueIds(descriptor.outputs, descriptor.typeId, "output");
        requireUniqueIds(descriptor.parameters, descriptor.typeId);
    }
}

void testBuiltinDescriptorsUseSupportedMetadataShape()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();

    for (const auto& descriptor : registry.nodeTypes())
    {
        require(!descriptor.displayName.empty(), descriptor.typeId + " should have a nonempty display name");
        require(!descriptor.category.empty(), descriptor.typeId + " should have a nonempty category");

        for (const auto& input : descriptor.inputs)
            require(isLowerCamelIdentifier(input.id), descriptor.typeId + " input id should be lower camel identifier: " + input.id);

        for (const auto& output : descriptor.outputs)
            require(isLowerCamelIdentifier(output.id), descriptor.typeId + " output id should be lower camel identifier: " + output.id);

        for (const auto& parameter : descriptor.parameters)
            require(isLowerCamelIdentifier(parameter.id), descriptor.typeId + " parameter id should be lower camel identifier: " + parameter.id);
    }

    const auto& annotation = requireNodeType(registry, "talas.annotation");
    // Its Text is text, not an expression.
    require(annotation.parameters.size() == 1 && annotation.parameters.front().id == "text"
            && annotation.parameters.front().kind == talas::core::ParameterKind::Text,
        "talas.annotation should keep its Text as a text parameter, not an expression");
}

void testBuiltinDescriptorsExposeHonestRuntimeMaturity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::unordered_map<talas::core::NodeSupportLevel, std::size_t> counts;

    for (const auto& descriptor : registry.nodeTypes())
    {
        require(!descriptor.supportSummary.empty(), descriptor.typeId + " should explain its support level");
        ++counts[descriptor.supportLevel];
    }

    require(counts[talas::core::NodeSupportLevel::FullRuntime] == 55, "catalog should expose 55 full-runtime prototypes after AnalogSequencer maturity correction");
    require(counts[talas::core::NodeSupportLevel::FunctionalSubset] == 126, "catalog should expose 126 functional subsets including FrameSynchronizer and the ContextFreeGrammar");
    require(counts[talas::core::NodeSupportLevel::Placeholder] == 2, "catalog should retain only the two restricted-script compatibility placeholders");
    require(counts[talas::core::NodeSupportLevel::Utility] == 181, "catalog should expose a hundred and eighty-one Talas utilities, including the replaced two-input Mixer, the CDP ports, the Pasted Sound Expression and the taps a DiskRecorder, the MIDI outputs and a MIDIFileEcho become within a Sound");
    require(
        requireNodeType(registry, "talas.filter").supportLevel == talas::core::NodeSupportLevel::FullRuntime,
        "filter should expose full runtime coverage");
    require(
        requireNodeType(registry, "talas.oscillator").supportLevel == talas::core::NodeSupportLevel::FullRuntime,
        "oscillator should expose its bounded full-runtime profile");
    require(
        requireNodeType(registry, "talas.fft").supportLevel == talas::core::NodeSupportLevel::FunctionalSubset,
        "historical packed FFT adapter should leave placeholder maturity");
    require(
        requireNodeType(registry, "talas.frequencyScale").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "FrequencyScale should expose its bounded phase-vocoder subset");
    require(
        requireNodeType(registry, "talas.monotonizer").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "Monotonizer should expose its known-pitch retargeting subset");
    require(
        requireNodeType(registry, "talas.scaleVocoder").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "ScaleVocoder should expose its semitone interval filter-bank subset");
    require(
        requireNodeType(registry, "talas.tunableVocoder").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "TunableVocoder should expose its relative linear/log filter-bank subset");
    require(
        requireNodeType(registry, "talas.vocoder").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "Vocoder should expose its fixed linear filter-bank subset");
    require(
        requireNodeType(registry, "talas.midiMapper").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "MIDI Mapper should expose its bounded gate/velocity subset");
    require(
        requireNodeType(registry, "talas.forcedProcessorAssignment").supportLevel
            == talas::core::NodeSupportLevel::FunctionalSubset,
        "ForcedProcessorAssignment should expose portable logical placement");
    const auto& placement =
        requireNodeType(registry, "talas.forcedProcessorAssignment");
    for (const auto limitation : {
             std::string_view{"static-logical-processor-only"},
             std::string_view{"portable-serial-host-execution"},
             std::string_view{"hardware-affinity-unsupported"},
             std::string_view{"card-local-memory-unsupported"},
             std::string_view{"parallel-workers-unsupported"},
         })
    {
        require(
            std::ranges::find(
                placement.capabilities.limitations,
                limitation)
                != placement.capabilities.limitations.end(),
            "ForcedProcessorAssignment should expose its portable serial limitations");
    }
}

void testBuiltinDescriptorsExposeOrthogonalExecutionCapabilities()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    std::unordered_map<talas::core::ExecutionSupport, std::size_t>
        offlineCounts;
    std::unordered_map<talas::core::ExecutionSupport, std::size_t>
        realtimeCounts;

    for (const auto& descriptor : registry.nodeTypes())
    {
        ++offlineCounts[descriptor.capabilities.offline];
        ++realtimeCounts[
            descriptor.capabilities.preparedRealtime];
        require(
            !descriptor.capabilities.limitations.empty()
                || (descriptor.capabilities.offline
                        == talas::core::ExecutionSupport::Supported
                    && descriptor.capabilities.preparedRealtime
                        == talas::core::ExecutionSupport::Supported),
            descriptor.typeId
                + " should expose limitations for every partial execution axis");
    }

    require(
        offlineCounts[talas::core::ExecutionSupport::Supported] == 241
            && offlineCounts[talas::core::ExecutionSupport::Subset] == 123
            && offlineCounts[talas::core::ExecutionSupport::Unsupported] == 0,
        "offline capability counts should include the typed spectral selector");
    require(
        realtimeCounts[talas::core::ExecutionSupport::Supported] == 235
            && realtimeCounts[talas::core::ExecutionSupport::Subset] == 129
            && realtimeCounts[talas::core::ExecutionSupport::Unsupported] == 0,
        "prepared realtime capability counts should include the typed spectral selector");

    const auto& midiFile = requireNodeType(
        registry, "talas.midiFileEcho");
    require(
        midiFile.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && midiFile.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiFile.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && midiFile.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiFile.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && midiFile.capabilities.supportsReset
            && midiFile.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && midiFile.capabilities.outputBusLayouts.empty()
            && midiFile.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && midiFile.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                   midiFile.capabilities.limitations,
                   "preparation-time-smf-format-0-1-2-parsing")
                != midiFile.capabilities.limitations.end(),
        "talas.midiFileEcho should expose the exact C42 bounded terminal contract");

    const auto& globalController = requireNodeType(
        registry, "talas.soundToGlobalController");
    require(
        globalController.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && globalController.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && globalController.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && globalController.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && globalController.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && globalController.capabilities.supportsReset
            && globalController.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && globalController.capabilities.outputBusLayouts.empty()
            && globalController.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && globalController.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                   globalController.capabilities.limitations,
                   "capture-only-no-global-controller-mutation")
                != globalController.capabilities.limitations.end()
            && std::ranges::find(
                   globalController.capabilities.limitations,
                   "bounded-event-value-or-connected-finite-scalar-values")
                != globalController.capabilities.limitations.end()
            && std::ranges::find(
                   globalController.capabilities.limitations,
                   "static-or-connected-finite-scalar-values")
                == globalController.capabilities.limitations.end(),
        "talas.soundToGlobalController should expose the exact C48 bounded terminal contract");

    const auto& triggeredController = requireNodeType(
        registry, "talas.triggeredSoundToGlobalController");
    require(
        triggeredController.inputs.size() == 2
            && triggeredController.inputs[0].id == "value"
            && triggeredController.inputs[1].id == "trigger"
            && triggeredController.parameters.size() == 4
            && triggeredController.parameters[0].id == "generatedEvent"
            && triggeredController.parameters[1].id == "value"
            && triggeredController.parameters[2].id == "trigger"
            && triggeredController.parameters[3].id == "allowLiveOverride"
            && triggeredController.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && triggeredController.capabilities.supportsReset
            && std::ranges::find(
                   triggeredController.capabilities.limitations,
                   "allow-live-override-requires-explicit-host-route")
                != triggeredController.capabilities.limitations.end(),
        "triggered controller should expose bounded capture and the explicit host-route limitation");

    const auto& channeller = requireNodeType(registry, "talas.channeller");
    require(
        channeller.inputs.size() == 4
            && channeller.inputs[0].id == "audioIn"
            && channeller.inputs[0].required
            && channeller.inputs[0].type == talas::core::PortType::Audio
            && channeller.inputs[1].id == "rightIn"
            && !channeller.inputs[1].required
            && channeller.inputs[1].type == talas::core::PortType::Audio
            && channeller.inputs[2].id == "leftChannel"
            && !channeller.inputs[2].required
            && channeller.inputs[2].type == talas::core::PortType::Control
            && channeller.inputs[2].signal.rate
                == talas::core::PortSignalRate::Sample
            && channeller.inputs[3].id == "rightChannel"
            && !channeller.inputs[3].required
            && channeller.inputs[3].type == talas::core::PortType::Control
            && channeller.inputs[3].signal.rate
                == talas::core::PortSignalRate::Sample
            && channeller.outputs.size() == 2
            && channeller.outputs[0].id == "leftOut"
            && channeller.outputs[1].id == "rightOut"
            && channeller.parameters.size() == 2
            && channeller.parameters[0].id == "leftChannel"
            && channeller.parameters[0].kind
                == talas::core::ParameterKind::Boolean
            && channeller.parameters[0].defaultBoolean == true
            && channeller.parameters[1].id == "rightChannel"
            && channeller.parameters[1].kind
                == talas::core::ParameterKind::Boolean
            && channeller.parameters[1].defaultBoolean == true
            && channeller.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && channeller.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && channeller.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !channeller.capabilities.supportsReset
            && channeller.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && channeller.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && channeller.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && channeller.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Stereo}
            && std::ranges::find(
                   channeller.capabilities.limitations,
                   "offline-and-prepared-shared-checked-finite-bit-exact-binary64-live-channel-selection")
                != channeller.capabilities.limitations.end()
            && std::ranges::find(
                   channeller.capabilities.limitations,
                   "compiler-owned-exact-plan-audio-selector-fallback-dual-output-buffer-and-zero-latency-authority")
                != channeller.capabilities.limitations.end()
            && std::ranges::find(
                   channeller.capabilities.limitations,
                   "prepared-realtime-static-or-per-frame-finite-channel-selectors")
                == channeller.capabilities.limitations.end(),
        "talas.channeller should expose the exact C133 live-selector contract");

    const auto& midiNote = requireNodeType(
        registry, "talas.midiOutputEvent");
    require(
        midiNote.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && midiNote.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiNote.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && midiNote.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiNote.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && midiNote.capabilities.supportsReset
            && midiNote.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && midiNote.capabilities.outputBusLayouts.empty()
            && midiNote.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && midiNote.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                   midiNote.capabilities.limitations,
                   "gate-edge-active-note-channel-ownership")
                != midiNote.capabilities.limitations.end(),
        "talas.midiOutputEvent should expose the exact C41 bounded terminal contract");

    const auto& midiController = requireNodeType(
        registry, "talas.midiOutputController");
    require(
        midiController.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && midiController.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiController.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && midiController.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiController.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && midiController.capabilities.supportsReset
            && midiController.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && midiController.capabilities.outputBusLayouts.empty()
            && midiController.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && midiController.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                   midiController.capabilities.limitations,
                   "three-byte-midi-control-change-events")
                != midiController.capabilities.limitations.end(),
        "talas.midiOutputController should expose the exact C40 bounded terminal contract");

    const auto& rawMidi = requireNodeType(
        registry, "talas.midiOutputEventInBytes");
    require(
        rawMidi.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && rawMidi.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && rawMidi.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && rawMidi.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && rawMidi.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && rawMidi.capabilities.supportsReset
            && rawMidi.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && rawMidi.capabilities.outputBusLayouts.empty()
            && rawMidi.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None,
        "talas.midiOutputEventInBytes should expose the exact C39 terminal capture contract: offline="
            + std::to_string(static_cast<int>(rawMidi.capabilities.offline))
            + " offlineBinding=" + std::to_string(static_cast<int>(rawMidi.capabilities.offlineBinding))
            + " realtime=" + std::to_string(static_cast<int>(rawMidi.capabilities.preparedRealtime))
            + " realtimeBinding=" + std::to_string(static_cast<int>(rawMidi.capabilities.preparedRealtimeBinding))
            + " state=" + std::to_string(static_cast<int>(rawMidi.capabilities.state))
            + " reset=" + std::to_string(rawMidi.capabilities.supportsReset)
            + " latency=" + std::to_string(static_cast<int>(rawMidi.capabilities.latency.kind))
            + " buses=" + std::to_string(rawMidi.capabilities.outputBusLayouts.size())
            + " resources=" + std::to_string(static_cast<int>(rawMidi.capabilities.resourceAccess)));

    for (const auto typeId : {
             std::string_view{"talas.highShelvingFilter"},
             std::string_view{"talas.lowShelvingFilter"},
             std::string_view{"talas.presenceFilter"},
         })
    {
        const auto& filter = requireNodeType(registry, std::string{typeId});
        require(
            filter.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Supported
                && filter.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && filter.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && filter.capabilities.supportsReset
                && filter.capabilities.latency.kind
                    == talas::core::LatencyKind::None
                && filter.capabilities.outputBusLayouts
                    == std::vector{talas::core::AudioBusLayoutKind::Mono}
                && std::ranges::find(
                    filter.capabilities.limitations,
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority")
                    != filter.capabilities.limitations.end(),
            std::string{typeId}
                + " should expose the exact C192 prepared contract");
    }

    struct C17Contract
    {
        std::string_view typeId;
        talas::core::AudioBusLayoutKind layout;
    };
    for (const auto& contract : {
             C17Contract{
                 "talas.twoFormantElement",
                 talas::core::AudioBusLayoutKind::Mono},
             C17Contract{
                 "talas.twoFormantVoiceElement",
                 talas::core::AudioBusLayoutKind::Mono},
             C17Contract{
                 "talas.dualParallelTwoPoleFilter",
                 talas::core::AudioBusLayoutKind::Mono},
             C17Contract{
                 "talas.graphicEq",
                 talas::core::AudioBusLayoutKind::Mono},
         })
    {
        const auto& filter = requireNodeType(
            registry, std::string{contract.typeId});
        require(
            filter.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Supported
                && filter.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && filter.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && filter.capabilities.supportsReset
                && filter.capabilities.latency.kind
                    == talas::core::LatencyKind::None
                && filter.capabilities.outputBusLayouts
                    == std::vector{contract.layout},
            std::string{contract.typeId}
                + " should expose the exact C193 prepared contract");
        const std::vector<std::string> exactLimitations{
            "offline-functional-subset",
            "offline-and-prepared-shared-checked-finite-mono-c17-biquad-family",
            "exact-two-formant-two-formant-voice-static-pole-and-seven-band-profiles",
            "stateless-event-value-or-connected-formant-bandwidth-scale-frequency-and-band-level-controls",
            "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority",
            "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-phase-schedule-retry-reset-and-zero-frame-no-op",
            "stateful-variable-general-identifier-live-pole-typed-event-multichannel-alternate-coefficient-and-exact-reference-semantics-unsupported",
        };
        require(
            filter.supportLevel
                    == talas::core::NodeSupportLevel::FunctionalSubset
                && filter.capabilities.offline
                    == talas::core::ExecutionSupport::Subset
                && filter.capabilities.offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && filter.capabilities.nestedSounds
                    == talas::core::NestedSoundSupport::Expanded
                && !filter.capabilities.typedEvents
                && !filter.capabilities.transportAware
                && filter.capabilities.latency.fixedFrames == 0
                && filter.capabilities.latency.parameterId.empty()
                && filter.capabilities.latency.preparedRule
                    == talas::core::PreparedLatencyRule::None
                && filter.capabilities.resourceAccess
                    == talas::core::ResourceAccessMode::None
                && filter.capabilities.fanInLatencyPolicy
                    == talas::core::FanInLatencyPolicy::NotApplicable
                && filter.capabilities.limitations == exactLimitations
                && filter.typeVersion == "1.0.0"
                && filter.stateSchemaVersion == 1
                && filter.provenance.vendor == "Talas"
                && filter.provenance.origin
                    == talas::core::NodeOrigin::BuiltIn,
            std::string{contract.typeId}
                + " should expose exact C193 descriptor provenance");
        for (const auto limitation : {
                 std::string_view{"offline-and-prepared-shared-checked-finite-mono-c17-biquad-family"},
                 std::string_view{"exact-two-formant-two-formant-voice-static-pole-and-seven-band-profiles"},
                 std::string_view{"stateless-event-value-or-connected-formant-bandwidth-scale-frequency-and-band-level-controls"},
                 std::string_view{"compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority"},
                 std::string_view{"allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-phase-schedule-retry-reset-and-zero-frame-no-op"},
                 std::string_view{"stateful-variable-general-identifier-live-pole-typed-event-multichannel-alternate-coefficient-and-exact-reference-semantics-unsupported"},
             })
        {
            require(
                std::ranges::find(
                    filter.capabilities.limitations, limitation)
                    != filter.capabilities.limitations.end(),
                std::string{contract.typeId}
                    + " should expose the complete C193 limitation contract");
        }
    }

    const auto& harmonicResonator = requireNodeType(
        registry, "talas.harmonicResonator");
    require(
        harmonicResonator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && harmonicResonator.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && harmonicResonator.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && harmonicResonator.capabilities.supportsReset
            && harmonicResonator.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && harmonicResonator.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && harmonicResonator.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && harmonicResonator.capabilities.limitations
                == std::vector<std::string>{
                    "offline-functional-subset",
                    "offline-and-prepared-shared-checked-finite-mono-private-memory-harmonic-resonator",
                    "allpass-fractional-period-sixty-db-decay-static-brightness-and-live-decay-frequency-scale-profile",
                    "stateless-event-value-or-connected-decay-frequency-scale-and-bounded-history-authority",
                    "compiler-owned-exact-descriptor-plan-settings-private-sentinel-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-delay-brightness-schedule-retry-reset-and-zero-frame-no-op",
                    "dynamic-brightness-stateful-variable-general-identifier-named-shared-memory-unbounded-history-typed-event-multichannel-and-exact-reference-semantics-unsupported",
                },
        "talas.harmonicResonator should expose the exact C195 prepared authority contract");

    const auto& dynamicRangeController = requireNodeType(
        registry, "talas.dynamicRangeController");
    require(
        dynamicRangeController.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && dynamicRangeController.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && dynamicRangeController.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && dynamicRangeController.capabilities.supportsReset
            && dynamicRangeController.capabilities.latency.kind
                == talas::core::LatencyKind::VariableOrUnknown
            && dynamicRangeController.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && dynamicRangeController.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && dynamicRangeController.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::PreserveRelative
            && dynamicRangeController.capabilities.limitations
                == std::vector<std::string>{
                    "offline-functional-subset",
                    "offline-and-prepared-shared-checked-finite-mono-dynamic-range-controller",
                    "static-compressor-or-downward-expander-and-stateless-event-value-or-connected-ratio-threshold-attack-release-delay-gain-profile",
                    "bounded-integer-delay-variable-latency-preserve-relative-zero-latency-input-ancestry-authority",
                    "compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-envelope-delay-schedule-retry-reset-and-zero-frame-no-op",
                    "multiband-soft-knee-alternate-detector-stereo-linking-sidechain-filtering-unbounded-lookahead-stateful-variable-general-identifier-typed-event-multichannel-and-exact-reference-semantics-unsupported",
                },
        "talas.dynamicRangeController should expose the exact C196 prepared authority contract");

    const auto& quadOscillator = requireNodeType(
        registry, "talas.quadOscillator");
    require(
        quadOscillator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && quadOscillator.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && quadOscillator.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && quadOscillator.capabilities.supportsReset
            && quadOscillator.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && quadOscillator.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && quadOscillator.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && quadOscillator.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                quadOscillator.capabilities.limitations,
                "prepared-realtime-checked-summed-quadrature-strict-nyquist")
                != quadOscillator.capabilities.limitations.end()
            && std::ranges::find(
                quadOscillator.capabilities.limitations,
                "prepared-realtime-zero-latency-input-ancestry")
                != quadOscillator.capabilities.limitations.end(),
        "talas.quadOscillator should expose the exact C22 prepared contract");

    const auto& overlappingMixer = requireNodeType(
        registry, "talas.overlappingMixer");
    require(
        overlappingMixer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && overlappingMixer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && overlappingMixer.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !overlappingMixer.capabilities.supportsReset
            && overlappingMixer.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && overlappingMixer.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && overlappingMixer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && overlappingMixer.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                overlappingMixer.capabilities.limitations,
                "prepared-realtime-checked-four-input-stereo-zero-overlap")
                != overlappingMixer.capabilities.limitations.end()
            && std::ranges::find(
                overlappingMixer.capabilities.limitations,
                "prepared-realtime-zero-latency-input-ancestry")
                != overlappingMixer.capabilities.limitations.end(),
        "talas.overlappingMixer should expose the exact C23 prepared contract");

    const auto& oscillatorBank = requireNodeType(
        registry, "talas.oscillatorBank");
    require(
        oscillatorBank.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && oscillatorBank.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && oscillatorBank.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && oscillatorBank.capabilities.supportsReset
            && oscillatorBank.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && oscillatorBank.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && oscillatorBank.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && oscillatorBank.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                oscillatorBank.capabilities.limitations,
                "prepared-realtime-checked-maximum-4096-static-oscillators")
                != oscillatorBank.capabilities.limitations.end()
            && std::ranges::find(
                oscillatorBank.capabilities.limitations,
                "prepared-realtime-blank-sine-or-immutable-table-wavetable")
                != oscillatorBank.capabilities.limitations.end()
            && std::ranges::find(
                oscillatorBank.capabilities.limitations,
                "prepared-realtime-zero-latency-spectrum-or-paired-audio-ancestry")
                != oscillatorBank.capabilities.limitations.end(),
        "talas.oscillatorBank should expose the exact C24 prepared contract");

    const auto& syntheticSpectrum = requireNodeType(
        registry, "talas.syntheticSpectrumFromArray");
    require(
        syntheticSpectrum.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && syntheticSpectrum.capabilities.supportsReset
            && syntheticSpectrum.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && std::ranges::find(
                syntheticSpectrum.capabilities.limitations,
                "prepared-realtime-static-finite-array-common-prefix")
                != syntheticSpectrum.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrum.capabilities.limitations,
                "prepared-realtime-immutable-stateless-brace-array-expressions")
                != syntheticSpectrum.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrum.capabilities.limitations,
                "event-variable-stateful-definition-named-pitch-and-live-array-expressions-unsupported")
                != syntheticSpectrum.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrum.capabilities.limitations,
                "serialized-linear-pairs-update-one-partial-per-sample")
                != syntheticSpectrum.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrum.capabilities.limitations,
                "serialized-output-requires-full-array-count-no-logscale-no-bandwidths")
                != syntheticSpectrum.capabilities.limitations.end(),
        "C65 Spectrum source authority should declare its immutable expression and common-prefix boundary");

    const auto& syntheticSpectrumFromSounds = requireNodeType(
        registry, "talas.syntheticSpectrumFromSounds");
    require(
        syntheticSpectrumFromSounds.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && syntheticSpectrumFromSounds.capabilities
                    .preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && syntheticSpectrumFromSounds.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !syntheticSpectrumFromSounds.capabilities.supportsReset
            && syntheticSpectrumFromSounds.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && syntheticSpectrumFromSounds.capabilities.outputBusLayouts.empty()
            && syntheticSpectrumFromSounds.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && syntheticSpectrumFromSounds.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "legacy-spectrum-output-uses-callback-local-packed-audio-frame")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "serialized-linear-amplitude-frequency-pairs-across-callbacks")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "serialized-pairs-require-logscale-and-bandwidths-disabled")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-maximum-4096-spectrum-partials")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-static-log-scale-and-bandwidth-mode")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-block-constant-frame-zero-partial-count")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-aligned-audio-control-ancestry")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "legacy-spectrum-packed-window-before-scheduler-split")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-no-feedback-pair-coexistence")
                != syntheticSpectrumFromSounds.capabilities.limitations.end()
            && std::ranges::find(
                syntheticSpectrumFromSounds.capabilities.limitations,
                "prepared-realtime-early-packed-window-failure-consumes-published-event-queues-and-score")
                != syntheticSpectrumFromSounds.capabilities.limitations.end(),
        "talas.syntheticSpectrumFromSounds should expose the exact C29 prepared contract");

    const auto& warpedTimeIndex = requireNodeType(
        registry, "talas.warpedTimeIndex");
    require(
        warpedTimeIndex.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && warpedTimeIndex.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && warpedTimeIndex.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && warpedTimeIndex.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && warpedTimeIndex.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && warpedTimeIndex.capabilities.supportsReset
            && warpedTimeIndex.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && warpedTimeIndex.capabilities.outputBusLayouts.empty()
            && warpedTimeIndex.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && warpedTimeIndex.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                warpedTimeIndex.capabilities.limitations,
                "equal-2-through-4096-strictly-ascending-finite-seconds-point-arrays-whose-braced-elements-may-follow-event-values")
                != warpedTimeIndex.capabilities.limitations.end()
            && std::ranges::find(
                warpedTimeIndex.capabilities.limitations,
                "shared-checked-finite-convex-map-normalization-trigger-rate-and-playback-state-transition")
                != warpedTimeIndex.capabilities.limitations.end()
            && std::ranges::find(
                warpedTimeIndex.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-trigger-rate-and-morph-with-connected-precedence")
                != warpedTimeIndex.capabilities.limitations.end()
            && std::ranges::find(
                warpedTimeIndex.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != warpedTimeIndex.capabilities.limitations.end()
            && std::ranges::find(
                warpedTimeIndex.capabilities.limitations,
                "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset")
                != warpedTimeIndex.capabilities.limitations.end(),
        "talas.warpedTimeIndex should expose the exact C147 shared map authority contract");

    const auto& waveshaper = requireNodeType(registry, "talas.waveshaper");
    requirePorts(
        waveshaper.inputs,
        {{"audioIn", talas::core::PortType::Audio, true},
         {"rightIn", talas::core::PortType::Audio, false}},
        waveshaper.typeId, "input");
    requirePorts(
        waveshaper.outputs,
        {{"audioOut", talas::core::PortType::Audio, false},
         {"rightOut", talas::core::PortType::Audio, false}},
        waveshaper.typeId, "output");
    require(
        waveshaper.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && waveshaper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && waveshaper.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !waveshaper.capabilities.supportsReset
            && waveshaper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && waveshaper.capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>{
                    talas::core::AudioBusLayoutKind::Mono,
                    talas::core::AudioBusLayoutKind::Stereo}
            && waveshaper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && waveshaper.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                waveshaper.capabilities.limitations,
                "offline-and-prepared-static-polynomial-immutable-wavetable-or-live-mono-memory-writer")
                != waveshaper.capabilities.limitations.end()
            && std::ranges::find(
                waveshaper.capabilities.limitations,
                "prepared-realtime-allocation-free-stateless-shaping")
                != waveshaper.capabilities.limitations.end()
            && std::ranges::find(
                waveshaper.capabilities.limitations,
                "stereo-wav-shaping-function-two-independent-output-lanes-with-optional-right-input")
                != waveshaper.capabilities.limitations.end()
            && std::ranges::find(
                waveshaper.capabilities.limitations,
                "unique-prior-same-scope-memory-writer-function-with-frame-ordered-read-and-rollback")
                != waveshaper.capabilities.limitations.end(),
        "talas.waveshaper should expose the stereo stateless finite contract");

    const auto& multiplyingWaveshaper = requireNodeType(
        registry, "talas.multiplyingWaveshaper");
    require(
        multiplyingWaveshaper.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && multiplyingWaveshaper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && multiplyingWaveshaper.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !multiplyingWaveshaper.capabilities.supportsReset
            && multiplyingWaveshaper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && multiplyingWaveshaper.capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>{
                    talas::core::AudioBusLayoutKind::Mono}
            && multiplyingWaveshaper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && multiplyingWaveshaper.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                multiplyingWaveshaper.capabilities.limitations,
                "offline-and-prepared-finite-immutable-normalized-wavetable-product")
                != multiplyingWaveshaper.capabilities.limitations.end()
            && std::ranges::find(
                multiplyingWaveshaper.capabilities.limitations,
                "prepared-realtime-allocation-free-stateless-connected-positive-scale")
                != multiplyingWaveshaper.capabilities.limitations.end()
            && std::ranges::find(
                multiplyingWaveshaper.capabilities.limitations,
                "offline-and-prepared-two-input-latency-aligned-ancestry")
                != multiplyingWaveshaper.capabilities.limitations.end()
            && std::ranges::find(
                multiplyingWaveshaper.capabilities.limitations,
                "editor-detector-delay-file-memory-writer-and-domain-metadata-unsupported")
                != multiplyingWaveshaper.capabilities.limitations.end(),
        "talas.multiplyingWaveshaper should expose the exact C81 stateless finite product contract");

    const auto& inputOutputCharacteristic = requireNodeType(
        registry, "talas.inputOutputCharacteristic");
    require(
        inputOutputCharacteristic.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && inputOutputCharacteristic.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && inputOutputCharacteristic.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !inputOutputCharacteristic.capabilities.supportsReset
            && inputOutputCharacteristic.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && inputOutputCharacteristic.capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>{
                    talas::core::AudioBusLayoutKind::Mono}
            && inputOutputCharacteristic.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && inputOutputCharacteristic.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                inputOutputCharacteristic.capabilities.limitations,
                "offline-and-prepared-finite-step-linear-hermite-curve")
                != inputOutputCharacteristic.capabilities.limitations.end()
            && std::ranges::find(
                inputOutputCharacteristic.capabilities.limitations,
                "prepared-realtime-allocation-free-stateless-fixed-length-breakpoints")
                != inputOutputCharacteristic.capabilities.limitations.end()
            && std::ranges::find(
                inputOutputCharacteristic.capabilities.limitations,
                "non-finite-upstream-input-fails-closed-without-upstream-state-recovery")
                != inputOutputCharacteristic.capabilities.limitations.end()
            && std::ranges::find(
                inputOutputCharacteristic.capabilities.limitations,
                "bounded-val-array-members-inherit-nearest-scalar-event-values-with-child-shadowing")
                != inputOutputCharacteristic.capabilities.limitations.end()
            && std::ranges::find(
                inputOutputCharacteristic.capabilities.limitations,
                "general-smalltalk-array-expressions-mutable-length-and-exact-reference-spline-unsupported")
                != inputOutputCharacteristic.capabilities.limitations.end(),
        "talas.inputOutputCharacteristic should expose the bounded live breakpoint curve contract");

    const auto& scaleAndOffset = requireNodeType(
        registry, "talas.scaleAndOffset");
    const std::vector<std::string> scaleAndOffsetLimitations{
        "offline-functional-subset",
        "offline-and-prepared-shared-checked-finite-dual-control-lane-affine-transform",
        "static-or-connected-bounded-independent-left-and-right-scale-and-offset-precedence-profile",
        "absent-right-value-duplicates-left-input-before-independent-right-affine-transform",
        "zero-latency-align-stateless-no-reset-two-owned-control-output-authority",
        "compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority",
        "allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-finite-stage-and-fma-identity",
        "packed-stereo-sound-object-multichannel-unbounded-control-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-and-exact-reference-semantics-unsupported",
    };
    require(
        scaleAndOffset.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && scaleAndOffset.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && scaleAndOffset.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && scaleAndOffset.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !scaleAndOffset.capabilities.supportsReset
            && scaleAndOffset.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && scaleAndOffset.capabilities.outputBusLayouts.empty()
            && scaleAndOffset.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && scaleAndOffset.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && scaleAndOffset.capabilities.limitations
                == scaleAndOffsetLimitations,
        "talas.scaleAndOffset should expose the exact C199 dual-lane affine authority contract");

    const auto& interpolate = requireNodeType(registry, "talas.interpolate");
    require(
        interpolate.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && interpolate.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && interpolate.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && interpolate.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !interpolate.capabilities.supportsReset
            && interpolate.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && interpolate.capabilities.outputBusLayouts.empty()
            && interpolate.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && interpolate.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-dual-control-lane-convex-interpolation")
                != interpolate.capabilities.limitations.end()
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "static-or-connected-bounded-left-and-right-amount-precedence-profile")
                != interpolate.capabilities.limitations.end()
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "zero-latency-align-stateless-no-reset-two-owned-control-output-authority")
                != interpolate.capabilities.limitations.end()
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-resource-bundle-parameter-input-producer-source-buffer-output-scope-lifecycle-and-local-transaction-authority")
                != interpolate.capabilities.limitations.end()
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "allocation-free-prepared-callback-and-offline-render-two-lane-failure-atomicity-signed-zero-fma-and-convex-split-identity")
                != interpolate.capabilities.limitations.end()
            && std::ranges::find(
                interpolate.capabilities.limitations,
                "packed-stereo-audio-multichannel-unrestricted-expression-stateful-variable-typed-event-upstream-recovery-non-convex-and-exact-reference-semantics-unsupported")
                != interpolate.capabilities.limitations.end(),
        "talas.interpolate should expose the exact C198 Supported dual-lane authority contract");

    const auto& iteratedWaveshaper = requireNodeType(
        registry, "talas.iteratedWaveshaper");
    require(
        iteratedWaveshaper.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && iteratedWaveshaper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && iteratedWaveshaper.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !iteratedWaveshaper.capabilities.supportsReset
            && iteratedWaveshaper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && iteratedWaveshaper.capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>{
                    talas::core::AudioBusLayoutKind::Mono}
            && iteratedWaveshaper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && iteratedWaveshaper.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                iteratedWaveshaper.capabilities.limitations,
                "offline-and-prepared-finite-immutable-wavetable-feedback")
                != iteratedWaveshaper.capabilities.limitations.end()
            && std::ranges::find(
                iteratedWaveshaper.capabilities.limitations,
                "prepared-realtime-allocation-free-stateless-maximum-4096-iterations")
                != iteratedWaveshaper.capabilities.limitations.end()
            && std::ranges::find(
                iteratedWaveshaper.capabilities.limitations,
                "file-memory-writer-and-wavetable-domain-metadata-unsupported")
                != iteratedWaveshaper.capabilities.limitations.end(),
        "talas.iteratedWaveshaper should expose the exact C80 stateless finite feedback contract");

    const auto& spectrumInRam = requireNodeType(
        registry, "talas.spectrumInRam");
    require(
        spectrumInRam.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && spectrumInRam.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && spectrumInRam.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && spectrumInRam.capabilities.supportsReset
            && spectrumInRam.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && spectrumInRam.capabilities.outputBusLayouts.empty()
            && spectrumInRam.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && spectrumInRam.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "prepared-realtime-immutable-fixed-width-analysis-frames")
                != spectrumInRam.capabilities.limitations.end()
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "prepared-realtime-normalized-minus-one-to-one-time-index")
                != spectrumInRam.capabilities.limitations.end()
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "prepared-realtime-project-authored-analysis-frequency-and-default-partial-tail")
                != spectrumInRam.capabilities.limitations.end()
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "prepared-realtime-static-or-block-constant-frame-zero-controls")
                != spectrumInRam.capabilities.limitations.end()
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "serialized-amplitude-frequency-pairs-feed-oscillator-bank")
                != spectrumInRam.capabilities.limitations.end()
            && std::ranges::find(
                spectrumInRam.capabilities.limitations,
                "analysis-file-decoding-automatic-metadata-scan-and-resynthesis-unsupported")
                != spectrumInRam.capabilities.limitations.end(),
        "talas.spectrumInRam should expose the exact C58 prepared contract");

    const auto& spectrumOnDisk = requireNodeType(
        registry, "talas.spectrumOnDisk");
    require(
        spectrumOnDisk.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && spectrumOnDisk.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && spectrumOnDisk.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && spectrumOnDisk.capabilities.supportsReset
            && spectrumOnDisk.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && spectrumOnDisk.capabilities.outputBusLayouts.empty()
            && spectrumOnDisk.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && spectrumOnDisk.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-immutable-table-snapshot")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-maximum-4096-spectrum-partials")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-static-or-block-constant-frame-zero-controls")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-immutable-fixed-width-frame-scan")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-logical-public-frame-rate-and-rising-edge-trigger")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-transactional-scan-state")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "prepared-realtime-project-authored-analysis-frequency-and-default-partial-tail")
                != spectrumOnDisk.capabilities.limitations.end()
            && std::ranges::find(
                spectrumOnDisk.capabilities.limitations,
                "real-disk-streaming-decoding-automatic-metadata-and-reverse-unsupported")
                != spectrumOnDisk.capabilities.limitations.end(),
        "talas.spectrumOnDisk should expose the exact C60 prepared contract");

    const auto& spectrumVoicedUnvoiced = requireNodeType(
        registry, "talas.spectrumVoicedUnvoiced");
    require(
        spectrumVoicedUnvoiced.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && spectrumVoicedUnvoiced.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && spectrumVoicedUnvoiced.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && spectrumVoicedUnvoiced.capabilities.supportsReset
            && spectrumVoicedUnvoiced.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && spectrumVoicedUnvoiced.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && spectrumVoicedUnvoiced.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && spectrumVoicedUnvoiced.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::PreserveRelative
            && std::ranges::find(
                spectrumVoicedUnvoiced.capabilities.limitations,
                "prepared-realtime-preserved-bounded-harmonicity-classifier")
                != spectrumVoicedUnvoiced.capabilities.limitations.end()
            && std::ranges::find(
                spectrumVoicedUnvoiced.capabilities.limitations,
                "prepared-realtime-transactional-one-public-frame-voiced-unvoiced-transition")
                != spectrumVoicedUnvoiced.capabilities.limitations.end()
            && std::ranges::find(
                spectrumVoicedUnvoiced.capabilities.limitations,
                "native-analysis-voicing-metadata-and-longer-smoothing-unsupported")
                != spectrumVoicedUnvoiced.capabilities.limitations.end(),
        "talas.spectrumVoicedUnvoiced should expose the exact C57 prepared contract");

    const auto& gaOscillators = requireNodeType(
        registry, "talas.gaOscillators");
    require(
        gaOscillators.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && gaOscillators.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && gaOscillators.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && gaOscillators.capabilities.supportsReset
            && gaOscillators.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && gaOscillators.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && gaOscillators.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && gaOscillators.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                gaOscillators.capabilities.limitations,
                "prepared-realtime-immutable-analysis-table-snapshots")
                != gaOscillators.capabilities.limitations.end()
            && std::ranges::find(
                gaOscillators.capabilities.limitations,
                "prepared-realtime-maximum-4096-additive-partials")
                != gaOscillators.capabilities.limitations.end()
            && std::ranges::find(
                gaOscillators.capabilities.limitations,
                "prepared-realtime-per-frame-envelope-frequency-and-morph-controls")
                != gaOscillators.capabilities.limitations.end()
            && std::ranges::find(
                gaOscillators.capabilities.limitations,
                "time-index-over-constant-computed-analysis-envelopes-for-harmonic-amplitude-tables")
                != gaOscillators.capabilities.limitations.end()
            && std::ranges::find(
                gaOscillators.capabilities.limitations,
                "ga-analysis-tables-of-adr-0027-play-complex-waveforms-their-envelopes-and-the-shared-deviation")
                != gaOscillators.capabilities.limitations.end(),
        "talas.gaOscillators should expose the exact C32 prepared contract");

    const auto& genericSource = requireNodeType(
        registry, "talas.genericSource");
    require(
        genericSource.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && genericSource.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && genericSource.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && genericSource.capabilities.supportsReset
            && genericSource.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && genericSource.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && genericSource.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && genericSource.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "prepared-realtime-live-input-disk-track-and-ram-sample-source-modes")
                != genericSource.capabilities.limitations.end()
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "live-input-channels-1-and-2-at-scale-without-the-sample-envelope")
                != genericSource.capabilities.limitations.end()
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "prepared-realtime-immutable-maximum-1048576-sample-snapshot")
                != genericSource.capabilities.limitations.end()
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "prepared-realtime-per-frame-trigger-envelope-frequency-and-scale-controls")
                != genericSource.capabilities.limitations.end()
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "prepared-realtime-rising-edge-replay-and-optional-autoloop")
                != genericSource.capabilities.limitations.end()
            && std::ranges::find(
                genericSource.capabilities.limitations,
                "disk-stream-and-mutable-sample-sources-unsupported")
                != genericSource.capabilities.limitations.end(),
        "talas.genericSource should expose the exact C33 prepared contract");

    const auto& sample = requireNodeType(registry, "talas.sample");
    require(
        sample.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && sample.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sample.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && sample.capabilities.supportsReset
            && sample.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && sample.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && sample.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && sample.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                sample.capabilities.limitations,
                "prepared-realtime-immutable-maximum-1048576-sample-snapshot")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "prepared-realtime-static-normalized-playback-and-loop-ranges")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "prepared-realtime-per-frame-gate-trigger-frequency-envelope-and-scale-controls")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "prepared-realtime-mono-gate-release-trigger-replay-and-optional-loop-fade")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "offline-and-prepared-same-sound-named-memory-writer-reader-maximum-1048576-frames")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "prepared-realtime-file-stream-sources-unsupported")
                != sample.capabilities.limitations.end()
            && std::ranges::find(
                sample.capabilities.limitations,
                "cross-sound-memory-writer-and-multichannel-sources-unsupported")
                != sample.capabilities.limitations.end(),
        "talas.sample should expose the exact C34 prepared contract");

    const auto& multisample = requireNodeType(
        registry, "talas.multisample");
    require(
        multisample.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && multisample.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multisample.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && multisample.capabilities.supportsReset
            && multisample.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && multisample.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && multisample.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && requireParameter(
                    registry, "talas.multisample", "samples")
                    .resourceAuthority
                == talas::core::ParameterResourceAuthority::StaticResourceList
            && multisample.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                multisample.capabilities.limitations,
                "prepared-realtime-static-sample-list-maximum-256-positions-4096-bytes")
                != multisample.capabilities.limitations.end()
            && std::ranges::find(
                multisample.capabilities.limitations,
                "prepared-realtime-deduplicated-immutable-maximum-256-tables-1048576-samples")
                != multisample.capabilities.limitations.end()
            && std::ranges::find(
                multisample.capabilities.limitations,
                "prepared-realtime-per-frame-frequency-gate-index-scale-and-envelope-controls")
                != multisample.capabilities.limitations.end()
            && std::ranges::find(
                multisample.capabilities.limitations,
                "prepared-realtime-index-at-gate-start-signed-playback-and-static-loop-window")
                != multisample.capabilities.limitations.end()
            && std::ranges::find(
                multisample.capabilities.limitations,
                "disk-dynamic-list-default-pitch-and-multichannel-sources-unsupported")
                != multisample.capabilities.limitations.end(),
        "talas.multisample should expose the exact C35 prepared contract");

    const auto& keyMappedMultisample = requireNodeType(
        registry, "talas.keyMappedMultisample");
    require(
        keyMappedMultisample.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && keyMappedMultisample.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && keyMappedMultisample.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && keyMappedMultisample.capabilities.supportsReset
            && keyMappedMultisample.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && keyMappedMultisample.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && keyMappedMultisample.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && keyMappedMultisample.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                keyMappedMultisample.capabilities.limitations,
                "prepared-realtime-immutable-bank-of-up-to-128-tables-of-1048576-samples")
                != keyMappedMultisample.capabilities.limitations.end()
            && std::ranges::find(
                keyMappedMultisample.capabilities.limitations,
                "one-per-half-step-equally-spaced-by-base-pitch-and-by-pitch-range-mappings-of-the-first-sample-folder")
                != keyMappedMultisample.capabilities.limitations.end()
            && std::ranges::find(
                keyMappedMultisample.capabilities.limitations,
                "prepared-realtime-per-frame-frequency-range-gate-gain-and-envelope-controls")
                != keyMappedMultisample.capabilities.limitations.end()
            && std::ranges::find(
                keyMappedMultisample.capabilities.limitations,
                "prepared-realtime-fixed-or-440-hz-relative-playback-and-whole-table-loop")
                != keyMappedMultisample.capabilities.limitations.end()
            && std::ranges::find(
                keyMappedMultisample.capabilities.limitations,
                "disk-stream-and-multichannel-sources-unsupported")
                != keyMappedMultisample.capabilities.limitations.end(),
        "talas.keyMappedMultisample should expose the exact C36 prepared contract");

    const auto& multifileDiskPlayer = requireNodeType(
        registry, "talas.multifileDiskPlayer");
    require(
        multifileDiskPlayer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && multifileDiskPlayer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multifileDiskPlayer.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && multifileDiskPlayer.capabilities.supportsReset
            && multifileDiskPlayer.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && multifileDiskPlayer.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && multifileDiskPlayer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && requireParameter(
                    registry, "talas.multifileDiskPlayer", "fileNames")
                    .resourceAuthority
                == talas::core::ParameterResourceAuthority::StaticResourceList
            && multifileDiskPlayer.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "prepared-realtime-static-file-list-maximum-256-positions-4096-bytes")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "prepared-realtime-deduplicated-immutable-maximum-256-tables-1048576-samples")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "prepared-realtime-per-frame-index-trigger-and-signed-rate-controls")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "prepared-realtime-trigger-selected-interpolated-gated-or-release-through-playback")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "offline-and-prepared-shared-selection-cursor-reset-and-failure-atomicity")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "connected-index-trigger-and-rate-own-inert-direct-fallbacks")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "compiler-owned-exact-prepared-plan-and-immutable-table-authority")
                != multifileDiskPlayer.capabilities.limitations.end()
            && std::ranges::find(
                multifileDiskPlayer.capabilities.limitations,
                "disk-stream-decoding-dynamic-list-sample-rate-metadata-and-multichannel-unsupported")
                != multifileDiskPlayer.capabilities.limitations.end(),
        "talas.multifileDiskPlayer should expose the exact C37 breadth and C106 lifecycle contract");

    const auto& memoryWriter = requireNodeType(registry, "talas.memoryWriter");
    require(
        memoryWriter.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && memoryWriter.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && memoryWriter.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && memoryWriter.capabilities.supportsReset
            && memoryWriter.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && memoryWriter.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && memoryWriter.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && memoryWriter.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "offline-and-prepared-mono-monitor-and-bounded-named-recording")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "prepared-realtime-positive-duration-maximum-1048576-frames")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "offline-and-prepared-zero-duration-full-capture-for-an-enclosing-set-duration-or-1048576-frames")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "prepared-realtime-static-or-per-frame-trigger-cyclic-and-silent-modes")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "offline-and-prepared-same-sound-named-capture-and-cyclic-overwrite-maximum-1048576-frames")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "live-sample-sample-cloud-function-generator-and-waveshaper-readers-with-complete-rollback")
                != memoryWriter.capabilities.limitations.end()
            && std::ranges::find(
                memoryWriter.capabilities.limitations,
                "global-storage-cross-sound-readers-event-scheduling-and-persistence-unsupported")
                != memoryWriter.capabilities.limitations.end(),
        "talas.memoryWriter should expose the exact C38 prepared contract");

    const auto& formantBank = requireNodeType(
        registry, "talas.formantBank");
    require(
        formantBank.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && formantBank.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && formantBank.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && formantBank.capabilities.supportsReset
            && formantBank.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && formantBank.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && formantBank.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && formantBank.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                formantBank.capabilities.limitations,
                "prepared-realtime-checked-maximum-4096-static-formants")
                != formantBank.capabilities.limitations.end()
            && std::ranges::find(
                formantBank.capabilities.limitations,
                "prepared-realtime-immutable-mono-impulse-response-triangular-fallback-and-bounded-overlap")
                != formantBank.capabilities.limitations.end()
            && std::ranges::find(
                formantBank.capabilities.limitations,
                "prepared-realtime-zero-latency-spectrum-frequency-ancestry")
                != formantBank.capabilities.limitations.end()
            && std::ranges::find(
                formantBank.capabilities.limitations,
                "serialized-direct-linear-synthetic-or-inram-source-count-independent-of-selected-formants")
                != formantBank.capabilities.limitations.end(),
        "talas.formantBank should expose the exact C25 prepared contract");

    const auto& formantBankOscillator = requireNodeType(
        registry, "talas.formantBankOscillator");
    require(
        formantBankOscillator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && formantBankOscillator.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && formantBankOscillator.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && formantBankOscillator.capabilities.supportsReset
            && formantBankOscillator.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && formantBankOscillator.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && formantBankOscillator.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && formantBankOscillator.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                formantBankOscillator.capabilities.limitations,
                "prepared-realtime-checked-selected-maximum-4096-static-formants")
                != formantBankOscillator.capabilities.limitations.end()
            && std::ranges::find(
                formantBankOscillator.capabilities.limitations,
                "prepared-realtime-required-mono-cascade-immutable-impulse-and-bounded-overlap")
                != formantBankOscillator.capabilities.limitations.end()
            && std::ranges::find(
                formantBankOscillator.capabilities.limitations,
                "prepared-realtime-static-or-connected-positive-pulse-frequency-independent-formant-centers-and-optional-dc")
                != formantBankOscillator.capabilities.limitations.end()
            && std::ranges::find(
                formantBankOscillator.capabilities.limitations,
                "prepared-realtime-zero-latency-spectrum-cascade-frequency-ancestry")
                != formantBankOscillator.capabilities.limitations.end()
            && std::ranges::find(
                formantBankOscillator.capabilities.limitations,
                "serialized-audio-pair-zero-bandwidth-extension-does-not-fulfill-reference-bandwidth-requirement")
                != formantBankOscillator.capabilities.limitations.end(),
        "talas.formantBankOscillator should expose the exact C26 prepared contract");

    const auto& spectrumModifier = requireNodeType(
        registry, "talas.spectrumModifier");
    require(
        spectrumModifier.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && spectrumModifier.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && spectrumModifier.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && spectrumModifier.capabilities.supportsReset
            && spectrumModifier.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && spectrumModifier.capabilities.outputBusLayouts.empty()
            && spectrumModifier.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && spectrumModifier.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-maximum-4096-spectrum-partials")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-static-or-block-constant-selection-and-transform-controls")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-deterministic-live-bounded-seed")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-deterministic-public-frame-probability-stream")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-static-one-based-source-ordinal-probability-profile")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-variables-v2-remainder-track-profiles")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-bounded-4096-track-live-hysteresis-state")
                != spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-deterministic-live-bounded-seed-no-live-hysteresis-state")
                == spectrumModifier.capabilities.limitations.end()
            && std::ranges::find(
                spectrumModifier.capabilities.limitations,
                "prepared-realtime-zero-latency-spectrum-control-ancestry")
                != spectrumModifier.capabilities.limitations.end(),
        "talas.spectrumModifier should expose the exact C55 prepared state contract");

    for (const auto [typeId, layout] : {
             std::pair{
                 std::string_view{"talas.stereoInOutput4"},
                 talas::core::AudioBusLayoutKind::Discrete4},
             std::pair{
                 std::string_view{"talas.stereoInOutput8"},
                 talas::core::AudioBusLayoutKind::Discrete8},
         })
    {
        const auto& terminal = requireNodeType(
            registry, std::string{typeId});
        require(
            terminal.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Subset
                && terminal.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && terminal.capabilities.state
                    == talas::core::NodeStateModel::Stateless
                && !terminal.capabilities.supportsReset
                && terminal.capabilities.latency.kind
                    == talas::core::LatencyKind::None
                && terminal.capabilities.outputBusLayouts
                    == std::vector{layout}
                && terminal.capabilities.resourceAccess
                    == talas::core::ResourceAccessMode::None
                && terminal.capabilities.fanInLatencyPolicy
                    == talas::core::FanInLatencyPolicy::PreserveRelative
                && std::ranges::find(
                    terminal.capabilities.limitations,
                    "prepared-realtime-paired-mono-terminal-routing")
                    != terminal.capabilities.limitations.end()
                && std::ranges::find(
                    terminal.capabilities.limitations,
                    "prepared-realtime-root-terminal-only")
                    != terminal.capabilities.limitations.end()
                && std::ranges::find(
                    terminal.capabilities.limitations,
                    "prepared-realtime-zero-latency-ancestry")
                    != terminal.capabilities.limitations.end()
                && std::ranges::find(
                    terminal.capabilities.limitations,
                    "true-stereo-sound-object-unpacking-unsupported")
                    != terminal.capabilities.limitations.end(),
            std::string{typeId}
                + " should expose the exact C28 paired terminal contract");
    }

    const auto& vcf = requireNodeType(registry, "talas.vcf");
    require(
        vcf.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && vcf.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && vcf.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && vcf.capabilities.supportsReset
            && vcf.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && vcf.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && vcf.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && std::ranges::find(
                vcf.capabilities.limitations,
                "prepared-realtime-checked-four-pole-tpt-mono-only")
                != vcf.capabilities.limitations.end(),
        "talas.vcf should expose the exact C19 prepared contract");

    for (const auto typeId : {
             std::string_view{"talas.firFilter"},
         })
    {
        const auto& filter = requireNodeType(
            registry, std::string{typeId});
        const auto expectedResourceAccess =
            talas::core::ResourceAccessMode::None;
        require(
            filter.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Supported
                && filter.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && filter.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && filter.capabilities.supportsReset
                && filter.capabilities.latency.kind
                    == talas::core::LatencyKind::None
                && filter.capabilities.outputBusLayouts
                    == std::vector{
                        talas::core::AudioBusLayoutKind::Mono}
                && filter.capabilities.resourceAccess
                    == expectedResourceAccess
                && filter.capabilities.limitations
                    == std::vector<std::string>{
                        "offline-functional-subset",
                        "offline-and-prepared-shared-checked-finite-mono-static-fir-one-through-4096-taps",
                        "exact-fir-static-coefficient-text-or-reresonator-immutable-table-and-static-time-index-profile",
                        "compiler-sealed-expression-language-definitions-and-exact-resource-snapshot-identity",
                        "compiler-owned-exact-descriptor-plan-settings-parameter-input-producer-source-buffer-output-scope-state-reset-zero-latency-and-local-transaction-authority",
                        "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-retry-reset-and-zero-frame-no-op",
                        "dynamic-coefficients-stateful-variable-general-identifier-mutable-table-oversized-taps-typed-event-multichannel-and-exact-reference-semantics-unsupported",
                    },
            std::string{typeId}
                + " should expose the exact C194 prepared authority contract");
    }
    // ADR-0022: the REResonator is the time-varying lattice of an RE table.
    const auto& resonator = requireNodeType(registry, "talas.reResonator");
    require(
        resonator.capabilities.preparedRealtime == talas::core::ExecutionSupport::Supported
            && resonator.capabilities.state == talas::core::NodeStateModel::SessionStateful
            && resonator.capabilities.supportsReset
            && resonator.capabilities.latency.kind == talas::core::LatencyKind::None
            && resonator.capabilities.resourceAccess == talas::core::ResourceAccessMode::ImmutableRead
            && resonator.capabilities.limitations
                == std::vector<std::string>{
                    "offline-functional-subset",
                    "offline-and-prepared-shared-checked-finite-mono-all-pole-lattice-of-order-one-through-128",
                    "re-table-of-our-own-format-with-reflection-coefficients-and-per-sample-time-index",
                    "external-re-files-unsupported-and-legacy-fir-reading-before-format-63",
                },
        "talas.reResonator should expose its lattice contract");

    // ADR-0015 amendment: the ContextFreeGrammar expands when compiling and
    // is no script runtime either.
    const auto& grammar = requireNodeType(registry, "talas.contextFreeGrammar");
    require(
        grammar.supportLevel == talas::core::NodeSupportLevel::FunctionalSubset
            && std::ranges::find(grammar.capabilities.limitations, "arbitrary-script-runtime-unsupported")
                != grammar.capabilities.limitations.end(),
        "talas.contextFreeGrammar should be a functional subset with no script-runtime claim");
    for (const auto typeId : {
             std::string_view{"talas.limeInterpreter"},
             std::string_view{"talas.script"},
         })
    {
        const auto& descriptor = requireNodeType(
            registry,
            std::string(typeId));
        require(
            descriptor.supportLevel
                    == talas::core::NodeSupportLevel::Placeholder
                && std::ranges::find(
                       descriptor.capabilities.limitations,
                       "arbitrary-script-runtime-unsupported")
                    != descriptor.capabilities.limitations.end(),
            std::string(typeId)
                + " must remain a compatibility placeholder with no arbitrary script-runtime claim");
    }

    for (const auto nodeId : {
             std::string_view{"talas.variable"},
             std::string_view{"talas.soundCollectionVariable"}})
    {
        const auto& selector = requireNodeType(
            registry,
            std::string(nodeId));
        require(
            selector.inputs.size()
                    == talas::project::maximumProjectSoundVariableCandidates
                && selector.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && selector.capabilities.supportsReset
                && selector.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Subset
                && selector.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && std::ranges::all_of(
                    selector.inputs,
                    [](const auto& port) {
                        return port.compilerPopulated
                            && !port.required
                            && port.type == talas::core::PortType::Audio
                            && port.id.starts_with("candidate");
                    }),
            std::string(nodeId)
                + " should retain resettable prepared selector authority and exactly 32 hidden compiler-populated candidate inputs");
    }

    const auto& grainCloud = requireNodeType(registry, "talas.grainCloud");
    require(
        grainCloud.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && grainCloud.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && grainCloud.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && grainCloud.capabilities.supportsReset
            && grainCloud.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "GrainCloud should expose its resettable zero-latency prepared realtime subset");

    const auto& chopper = requireNodeType(registry, "talas.chopper");
    require(
        chopper.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && chopper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && chopper.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && chopper.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && chopper.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && chopper.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && chopper.capabilities.supportsReset
            && chopper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && chopper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && chopper.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && chopper.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && chopper.capabilities.limitations
                == std::vector<std::string>{
                    "finite-immutable-plain-table-linear-half-open-cycle-interpolation",
                    "static-seconds-event-or-per-frame-connected-positive-finite-duration-controls-with-independent-connected-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-pre-sample-cycle-shrink-and-wrap",
                    "compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-zero-frame-failure-atomic-transport-reset-and-partition-stable-retry",
                    "file-stream-memory-writer-overlap-randomized-density-spatial-multichannel-and-exact-reference-semantics-unsupported",
                },
        "C161 Chopper should expose exact Full Runtime mono immutable-table execution truth");

    const auto& functionGenerator = requireNodeType(
        registry, "talas.functionGenerator");
    require(
        functionGenerator.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && functionGenerator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && functionGenerator.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && functionGenerator.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && functionGenerator.capabilities.supportsReset
            && functionGenerator.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && functionGenerator.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && functionGenerator.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && functionGenerator.capabilities.outputBusLayouts.empty()
            && functionGenerator.capabilities.limitations
                == std::vector<std::string>{
                    "finite-immutable-plain-table-or-unique-prior-same-scope-memory-writer-mono-capture-linear-one-shot-interpolation",
                    "static-scalar-event-or-per-frame-connected-finite-trigger-and-positive-seconds-duration-with-independent-connected-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-and-duration-retain-resume",
                    "finite-scalar-or-per-frame-mono-sound-level-with-connected-precedence",
                    "compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset",
                    "other-memory-writer-routes-file-metadata-multichannel-alternate-interpolation-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported",
                },
        "C155 Function Generator should expose exact Full Runtime table and named-memory execution truth");

    const auto& multislope = requireNodeType(
        registry, "talas.multislopeFunctionGenerator");
    require(
        multislope.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && multislope.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && multislope.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && multislope.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && multislope.capabilities.supportsReset
            && multislope.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && multislope.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && multislope.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && multislope.capabilities.outputBusLayouts.empty()
            && multislope.capabilities.limitations
                == std::vector<std::string>{
                    "finite-static-or-hot-brace-list-checked-piecewise-linear-one-shot-envelope-with-segments-taking-hot-values-as-they-start",
                    "static-scalar-event-or-per-frame-connected-finite-level-gate-and-positive-rate-with-independent-connected-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-falling-edge-continuation-and-same-frame-control-ordering",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset",
                    "list-length-changes-loops-nonlinear-segments-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported",
                },
        "C156 Multislope should expose exact Full Runtime resource-free execution truth");

    const auto& multisegment = requireNodeType(
        registry, "talas.multisegmentEnvelope");
    require(
        multisegment.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && multisegment.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && multisegment.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multisegment.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && multisegment.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && multisegment.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && multisegment.capabilities.supportsReset
            && multisegment.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && multisegment.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && multisegment.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && multisegment.capabilities.outputBusLayouts.empty()
            && multisegment.capabilities.limitations
                == std::vector<std::string>{
                    "finite-static-or-hot-brace-list-checked-piecewise-linear-envelope-with-segments-taking-hot-values-as-they-start-and-slope-preserving-loop-drift",
                    "static-scalar-event-or-per-frame-connected-finite-unit-level-and-finite-gate-with-independent-connected-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-rising-edge-retrigger-falling-edge-continuation-and-same-frame-control-ordering",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-static-loop-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset",
                    "list-length-changes-dynamic-loops-proprietary-release-lifecycle-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported",
                },
        "C157 Multisegment should expose exact Full Runtime resource-free static-loop execution truth");

    const auto& graphical = requireNodeType(
        registry, "talas.graphicalEnvelope");
    require(
        graphical.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && graphical.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && graphical.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && graphical.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && graphical.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && graphical.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && graphical.capabilities.supportsReset
            && graphical.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && graphical.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && graphical.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && graphical.capabilities.outputBusLayouts.empty()
            && graphical.capabilities.limitations
                == std::vector<std::string>{
                    "finite-static-alternating-time-value-checked-piecewise-linear-envelope-with-loop-marks-repeating-while-gate-is-on",
                    "static-scalar-event-or-per-frame-connected-finite-unit-level-finite-gate-and-positive-rate-with-independent-connected-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-level-before-output-gate-retrigger-rate-after-output-and-falling-edge-continuation",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset",
                    "graphical-editor-metadata-hot-shape-expressions-nonlinear-segments-multichannel-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported",
                },
        "C158 Graphical Envelope should expose exact Full Runtime resource-free one-shot execution truth");

    const auto& noise = requireNodeType(registry, "talas.noise");
    require(
        noise.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && noise.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && noise.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && noise.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && noise.capabilities.supportsReset
            && noise.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && noise.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && noise.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && noise.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                noise.capabilities.limitations,
                "offline-and-prepared-shared-checked-white-pink-and-hot-pink-transition")
                != noise.capabilities.limitations.end()
            && std::ranges::find(
                noise.capabilities.limitations,
                "compiler-owned-exact-plan-settings-control-bindings-and-buffer-authority")
                != noise.capabilities.limitations.end()
            && std::ranges::find(
                noise.capabilities.limitations,
                "clean-room-generator-algorithm-exact-reference-statistical-seed-phase-and-sound-parity-unsupported")
                != noise.capabilities.limitations.end(),
        "C118 Noise should expose supported resettable zero-latency mono resource-free clean-room execution truth");

    const auto& absoluteValue = requireNodeType(
        registry, "talas.absoluteValue");
    require(
        absoluteValue.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && absoluteValue.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && absoluteValue.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && absoluteValue.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !absoluteValue.capabilities.supportsReset
            && absoluteValue.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && absoluteValue.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && absoluteValue.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && absoluteValue.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                absoluteValue.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-magnitude")
                != absoluteValue.capabilities.limitations.end()
            && std::ranges::find(
                absoluteValue.capabilities.limitations,
                "compiler-owned-exact-plan-input-output-buffer-and-zero-latency-authority")
                != absoluteValue.capabilities.limitations.end()
            && std::ranges::find(
                absoluteValue.capabilities.limitations,
                "non-finite-complex-vector-audio-bus-script-and-exact-reference-semantics-unsupported")
                != absoluteValue.capabilities.limitations.end(),
        "C119 Absolute Value should expose supported stateless zero-latency resource-free finite-magnitude execution truth");

    const auto& arcTan = requireNodeType(registry, "talas.arcTan");
    require(
        arcTan.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && arcTan.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && arcTan.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && arcTan.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !arcTan.capabilities.supportsReset
            && arcTan.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && arcTan.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && arcTan.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && arcTan.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                arcTan.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-platform-arctangent")
                != arcTan.capabilities.limitations.end()
            && std::ranges::find(
                arcTan.capabilities.limitations,
                "exact-signed-zero-subnormal-ordinary-and-maximum-finite-result-bits")
                != arcTan.capabilities.limitations.end()
            && std::ranges::find(
                arcTan.capabilities.limitations,
                "compiler-owned-exact-plan-input-output-buffer-and-zero-latency-authority")
                != arcTan.capabilities.limitations.end()
            && std::ranges::find(
                arcTan.capabilities.limitations,
                "non-finite-complex-vector-audio-bus-degree-script-alternate-libm-and-exact-reference-semantics-unsupported")
                != arcTan.capabilities.limitations.end(),
        "C120 ArcTan should expose supported stateless zero-latency resource-free finite platform-angle execution truth");

    const auto& equality = requireNodeType(registry, "talas.equality");
    require(
        equality.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && equality.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && equality.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && equality.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !equality.capabilities.supportsReset
            && equality.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && equality.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && equality.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && equality.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                equality.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-exact-binary64-equality")
                != equality.capabilities.limitations.end()
            && std::ranges::find(
                equality.capabilities.limitations,
                "exact-canonical-boolean-signed-zero-adjacent-subnormal-and-maximum-finite-result-bits")
                != equality.capabilities.limitations.end()
            && std::ranges::find(
                equality.capabilities.limitations,
                "compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority")
                != equality.capabilities.limitations.end()
            && std::ranges::find(
                equality.capabilities.limitations,
                "live-finite-non-negative-tolerance-field")
                != equality.capabilities.limitations.end()
            && std::ranges::find(
                equality.capabilities.limitations,
                "non-finite-ordered-complex-vector-string-audio-bus-script-and-exact-reference-semantics-unsupported")
                != equality.capabilities.limitations.end(),
        "C121 Equality should expose supported stateless zero-latency resource-free finite exact-comparison execution truth");

    const auto& setRange = requireNodeType(registry, "talas.setRange");
    require(
        setRange.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && setRange.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && setRange.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && setRange.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !setRange.capabilities.supportsReset
            && setRange.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && setRange.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && setRange.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && setRange.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                setRange.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-ordered-binary64-affine-mapping")
                != setRange.capabilities.limitations.end()
            && std::ranges::find(
                setRange.capabilities.limitations,
                "static-fallback-or-independent-connected-old-and-new-range-control-authority")
                != setRange.capabilities.limitations.end()
            && std::ranges::find(
                setRange.capabilities.limitations,
                "compiler-owned-exact-plan-parameter-optional-binding-buffer-and-zero-latency-authority")
                != setRange.capabilities.limitations.end()
            && std::ranges::find(
                setRange.capabilities.limitations,
                "non-finite-clamping-epsilon-range-sort-vector-complex-audio-bus-script-and-exact-reference-semantics-unsupported")
                != setRange.capabilities.limitations.end(),
        "C122 Set Range should expose supported stateless zero-latency resource-free finite ordered-affine execution truth");

    const auto& add = requireNodeType(registry, "talas.add");
    const auto exactPerFrameControl = [](
        const talas::core::PortDescriptor& port,
        std::string_view id,
        bool required) {
        return port.id == id
            && port.type == talas::core::PortType::Control
            && port.required == required
            && port.signal.rate == talas::core::PortSignalRate::Sample
            && port.signal.edge
                == talas::core::PortEdgeSemantics::Continuous
            && !port.signal.timelineBearing;
    };
    require(
        add.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && add.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && add.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && add.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !add.capabilities.supportsReset
            && add.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && add.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && add.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && add.capabilities.outputBusLayouts.empty()
            && add.inputs.size() == 2
            && exactPerFrameControl(add.inputs[0], "left", true)
            && exactPerFrameControl(add.inputs[1], "right", true)
            && add.outputs.size() == 1
            && exactPerFrameControl(
                add.outputs.front(), "value", false)
            && std::ranges::find(
                add.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-addition")
                != add.capabilities.limitations.end()
            && std::ranges::find(
                add.capabilities.limitations,
                "exact-operand-order-signed-zero-cancellation-subnormal-adjacent-and-maximum-finite-result-bits")
                != add.capabilities.limitations.end()
            && std::ranges::find(
                add.capabilities.limitations,
                "compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority")
                != add.capabilities.limitations.end()
            && std::ranges::find(
                add.capabilities.limitations,
                "non-finite-saturating-compensated-variadic-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported")
                != add.capabilities.limitations.end(),
        "C128 Add should expose supported stateless zero-latency resource-free checked per-frame binary64 addition truth");

    const auto& mixer = requireNodeType(registry, "talas.mixer");
    const auto exactPerFrameAudio = [](
        const talas::core::PortDescriptor& port,
        std::string_view id,
        bool required) {
        return port.id == id
            && port.type == talas::core::PortType::Audio
            && port.required == required
            && port.signal.rate == talas::core::PortSignalRate::Sample
            && port.signal.audioLayout
                == talas::core::AudioBusLayoutKind::Mono
            && port.signal.edge
                == talas::core::PortEdgeSemantics::Continuous
            && port.signal.timelineBearing;
    };
    require(
        mixer.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && mixer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && mixer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && mixer.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !mixer.capabilities.supportsReset
            && mixer.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && mixer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && mixer.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && mixer.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Mono}
            && mixer.inputs.size() == 4
            && exactPerFrameAudio(mixer.inputs[0], "in1", true)
            && exactPerFrameAudio(mixer.inputs[1], "in2", true)
            && exactPerFrameControl(mixer.inputs[2], "gain1", false)
            && exactPerFrameControl(mixer.inputs[3], "gain2", false)
            && mixer.outputs.size() == 1
            && exactPerFrameAudio(
                mixer.outputs.front(), "audioOut", false)
            && std::ranges::find(
                mixer.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-unfused-binary64-weighted-sum")
                != mixer.capabilities.limitations.end()
            && std::ranges::find(
                mixer.capabilities.limitations,
                "static-fallback-or-connected-finite-gains-with-connected-precedence")
                != mixer.capabilities.limitations.end()
            && std::ranges::find(
                mixer.capabilities.limitations,
                "compiler-owned-exact-plan-dual-audio-optional-gain-output-buffer-and-aligned-fan-in-authority")
                != mixer.capabilities.limitations.end()
            && std::ranges::find(
                mixer.capabilities.limitations,
                "non-finite-fma-saturating-normalized-variadic-stereo-multichannel-complex-vector-matrix-script-and-exact-reference-semantics-unsupported")
                != mixer.capabilities.limitations.end(),
        "C129 Mixer should expose supported stateless aligned-mono checked unfused weighted-sum truth");

    // The Mixer (ADR-0017) replaces the two-input Mixer in the
    // palette: its Inputs take many Sounds, Left and Right set the levels.
    const auto& talasMixer = requireNodeType(registry, "talas.mixerV2");
    const auto unitLevel = [](const talas::core::ParameterDescriptor& parameter, std::string_view id) {
        return parameter.id == id
            && parameter.kind == talas::core::ParameterKind::Expression
            && parameter.domain == talas::expr::ValueDomain::Scalar
            && parameter.defaultExpression == "1.0";
    };
    require(
        mixer.replacedBy == "talas.mixerV2"
            && mixer.displayName == "Two-Input Mixer"
            && talasMixer.replacedBy.empty()
            && talasMixer.displayName == "Mixer"
            && talasMixer.category == "Mixing & Panning"
            && talasMixer.supportLevel == talas::core::NodeSupportLevel::FullRuntime
            && talasMixer.capabilities.offline == talas::core::ExecutionSupport::Supported
            && talasMixer.capabilities.preparedRealtime == talas::core::ExecutionSupport::Supported
            && talasMixer.capabilities.state == talas::core::NodeStateModel::Stateless
            && talasMixer.capabilities.latency.kind == talas::core::LatencyKind::None
            && talasMixer.capabilities.fanInLatencyPolicy == talas::core::FanInLatencyPolicy::Align
            && talasMixer.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && talasMixer.inputs.size() == 4
            && exactPerFrameAudio(talasMixer.inputs[0], "inputs", true)
            && talasMixer.inputs[0].acceptsMany
            && exactPerFrameAudio(talasMixer.inputs[1], "rightInputs", false)
            && talasMixer.inputs[1].acceptsMany
            && exactPerFrameControl(talasMixer.inputs[2], "left", false)
            && exactPerFrameControl(talasMixer.inputs[3], "right", false)
            && !talasMixer.inputs[2].acceptsMany
            && !talasMixer.inputs[3].acceptsMany
            && talasMixer.outputs.size() == 2
            && exactPerFrameAudio(talasMixer.outputs[0], "leftOut", false)
            && exactPerFrameAudio(talasMixer.outputs[1], "rightOut", false)
            && talasMixer.parameters.size() == 2
            && unitLevel(talasMixer.parameters[0], "left")
            && unitLevel(talasMixer.parameters[1], "right"),
        "the Mixer should take many mono or stereo Inputs and set Left and Right levels");

    const auto& product = requireNodeType(registry, "talas.product");
    require(
        product.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && product.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && product.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && product.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !product.capabilities.supportsReset
            && product.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && product.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && product.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && product.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                product.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-multiplication")
                != product.capabilities.limitations.end()
            && std::ranges::find(
                product.capabilities.limitations,
                "exact-signed-zero-subnormal-underflow-and-maximum-finite-result-bits")
                != product.capabilities.limitations.end()
            && std::ranges::find(
                product.capabilities.limitations,
                "compiler-owned-exact-plan-any-number-of-inputs-output-buffer-and-zero-latency-authority")
                != product.capabilities.limitations.end()
            && std::ranges::find(
                product.capabilities.limitations,
                "non-finite-saturating-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported")
                != product.capabilities.limitations.end(),
        "C123 Product should expose supported stateless zero-latency resource-free finite binary64 multiplication truth");

    const auto& difference = requireNodeType(registry, "talas.difference");
    require(
        difference.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && difference.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && difference.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && difference.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !difference.capabilities.supportsReset
            && difference.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && difference.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && difference.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && difference.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                difference.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-subtraction")
                != difference.capabilities.limitations.end()
            && std::ranges::find(
                difference.capabilities.limitations,
                "exact-operand-order-signed-zero-cancellation-subnormal-adjacent-and-maximum-finite-result-bits")
                != difference.capabilities.limitations.end()
            && std::ranges::find(
                difference.capabilities.limitations,
                "compiler-owned-exact-plan-dual-input-output-buffer-and-zero-latency-authority")
                != difference.capabilities.limitations.end()
            && std::ranges::find(
                difference.capabilities.limitations,
                "non-finite-saturating-epsilon-complex-vector-matrix-unit-audio-bus-script-and-exact-reference-semantics-unsupported")
                != difference.capabilities.limitations.end(),
        "C124 Difference should expose supported stateless zero-latency resource-free finite binary64 subtraction truth");

    const auto& sqrtMagnitude = requireNodeType(
        registry, "talas.sqrtMagnitude");
    require(
        sqrtMagnitude.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && sqrtMagnitude.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && sqrtMagnitude.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sqrtMagnitude.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !sqrtMagnitude.capabilities.supportsReset
            && sqrtMagnitude.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && sqrtMagnitude.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && sqrtMagnitude.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && sqrtMagnitude.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                sqrtMagnitude.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-native-binary64-hypot")
                != sqrtMagnitude.capabilities.limitations.end()
            && std::ranges::find(
                sqrtMagnitude.capabilities.limitations,
                "exact-positive-magnitude-signed-zero-subnormal-boundary-and-finite-overrange-saturation")
                != sqrtMagnitude.capabilities.limitations.end()
            && std::ranges::find(
                sqrtMagnitude.capabilities.limitations,
                "compiler-owned-exact-plan-optional-dual-audio-input-output-buffer-and-zero-latency-authority")
                != sqrtMagnitude.capabilities.limitations.end()
            && std::ranges::find(
                sqrtMagnitude.capabilities.limitations,
                "complex-vector-matrix-multichannel-control-script-alternate-libm-and-exact-reference-semantics-unsupported")
                != sqrtMagnitude.capabilities.limitations.end(),
        "C125 Sqrt Magnitude should expose supported stateless zero-latency resource-free checked native-hypot truth");

    const auto& vca = requireNodeType(registry, "talas.vca");
    require(
        vca.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && vca.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && vca.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && vca.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !vca.capabilities.supportsReset
            && vca.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && vca.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && vca.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && vca.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                vca.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-native-binary64-multiplication")
                != vca.capabilities.limitations.end()
            && std::ranges::find(
                vca.capabilities.limitations,
                "static-live-expression-or-connected-finite-mono-gain-with-connected-precedence")
                != vca.capabilities.limitations.end()
            && std::ranges::find(
                vca.capabilities.limitations,
                "compiler-owned-exact-plan-parameter-input-output-buffer-and-zero-latency-authority")
                != vca.capabilities.limitations.end()
            && std::ranges::find(
                vca.capabilities.limitations,
                "non-finite-saturating-stereo-multichannel-complex-vector-matrix-script-and-exact-reference-semantics-unsupported")
                != vca.capabilities.limitations.end(),
        "C126 VCA should expose supported stateless zero-latency resource-free checked native-product truth");

    const auto& constant = requireNodeType(registry, "talas.constant");
    require(
        constant.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && constant.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && constant.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && constant.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !constant.capabilities.supportsReset
            && constant.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && constant.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && constant.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && constant.outputs.size() == 1
            && constant.outputs.front().id == "value"
            && constant.outputs.front().type
                == talas::core::PortType::Control
            && constant.outputs.front().signal.rate
                == talas::core::PortSignalRate::Sample
            && constant.outputs.front().signal.edge
                == talas::core::PortEdgeSemantics::Continuous
            && constant.outputs.front().signal.timelineBearing
            && std::ranges::find(
                constant.capabilities.limitations,
                "offline-and-prepared-shared-checked-finite-binary64-scalar-broadcast")
                != constant.capabilities.limitations.end()
            && std::ranges::find(
                constant.capabilities.limitations,
                "compiler-owned-exact-plan-parameter-expression-output-buffer-scope-and-zero-latency-authority")
                != constant.capabilities.limitations.end()
            && std::ranges::find(
                constant.capabilities.limitations,
                "non-finite-unit-polymorphic-audio-vector-matrix-complex-string-resource-script-random-and-exact-reference-semantics-unsupported")
                != constant.capabilities.limitations.end(),
        "C127 Constant should expose supported stateless zero-latency resource-free exact per-frame scalar-broadcast truth");

    const auto& midiMapper = requireNodeType(registry, "talas.midiMapper");
    require(
        midiMapper.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && midiMapper.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiMapper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && midiMapper.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && midiMapper.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && midiMapper.capabilities.supportsReset
            && midiMapper.capabilities.typedEvents
            && !midiMapper.capabilities.transportAware
            && midiMapper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && midiMapper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && std::ranges::find(
                   midiMapper.capabilities.limitations,
                   "offline-functional-subset")
                != midiMapper.capabilities.limitations.end()
            && std::ranges::find(
                   midiMapper.capabilities.limitations,
                   "prepared-realtime-subset")
                != midiMapper.capabilities.limitations.end(),
        "MIDI Mapper capability metadata should match its compiled bounded authority");

    const auto& analogSequencer = requireNodeType(
        registry, "talas.analogSequencer");
    require(
        analogSequencer.supportLevel
                == talas::core::NodeSupportLevel::FunctionalSubset
            && analogSequencer.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && analogSequencer.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && analogSequencer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && analogSequencer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && analogSequencer.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && analogSequencer.capabilities.supportsReset
            && analogSequencer.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && analogSequencer.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-maximum-256-static-step-mono-in-stereo-out-amplitude-sequencer",
                    "shorter-finite-static-sequences-repeat-final-value-through-longest-sequence",
                    "zero-based-start-end-indices-with-format-38-legacy-one-based-migration",
                    "static-or-event-value-or-per-frame-connected-bipolar-left-right-finite-gate-non-negative-integer-step-and-signed-nonzero-rate-controls-with-nonzero-step-within-selected-range-and-connected-precedence",
                    "prepared-realtime-allocation-free-reset-zero-frame-partition-stable-failure-atomic-retry",
                    "compiler-owned-exact-descriptor-plan-settings-parameter-binding-buffer-producer-scope-zero-latency-authority",
                    "key-mode-stage-sampled-note-arrays-live-or-connected-nonzero-rate-gate-step-duration-mask-timed-controls-named-extra-values-and-generated-index-no-exact-reference-semantics",
                }
            && analogSequencer.supportSummary
                == "Subset: up to 256 steps shape amplitude or sequence classic and modern key arrays with named ExtraValues and GeneratedIndex through up to 128 upstream voices.",
        "Analog Sequencer should publish supported execution capabilities and honest subset maturity");

    const auto& ambisonicDecoder = requireNodeType(
        registry, "talas.horizontalAmbisonicDecoder");
    require(
        ambisonicDecoder.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && ambisonicDecoder.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && ambisonicDecoder.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && ambisonicDecoder.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && ambisonicDecoder.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Discrete4,
                    talas::core::AudioBusLayoutKind::Discrete8,
                },
        "Horizontal Ambisonic Decoder should expose its bounded zero-latency discrete contract");

    const auto& sampleCloud = requireNodeType(registry, "talas.sampleCloud");
    require(
        sampleCloud.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && sampleCloud.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sampleCloud.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && sampleCloud.capabilities.supportsReset
            && sampleCloud.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "SampleCloud should expose its resettable zero-latency prepared realtime subset");

    const auto& cloudBank = requireNodeType(registry, "talas.cloudBank");
    require(
        cloudBank.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && cloudBank.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && cloudBank.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && cloudBank.capabilities.supportsReset
            && cloudBank.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "CloudBank should expose its resettable zero-latency prepared realtime subset");

    const auto& cloudBankElement = requireNodeType(
        registry, "talas.cloudBankElement");
    require(
        cloudBankElement.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && cloudBankElement.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && cloudBankElement.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && cloudBankElement.capabilities.supportsReset
            && cloudBankElement.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "CloudBankElement should expose its resettable zero-latency prepared realtime subset");

    const auto& oscillator = requireNodeType(registry, "talas.oscillator");
    require(
        oscillator.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && oscillator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && oscillator.capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && oscillator.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel,
        "oscillator should report its promoted shared-kernel execution binding");
    require(
        oscillator.capabilities.limitations
            == std::vector<std::string>{
                "offline-and-prepared-shared-checked-finite-sine-or-immutable-cyclic-wavetable-oscillator-kernel",
                "static-or-direct-event-value-or-connected-selected-finite-frequency-amplitude-envelope-pitch-bend-reset-and-audio-rate-fm-that-wraps-the-phase-and-aliases-past-nyquist",
                "sample-accurate-direct-event-value-scheduling-with-positive-reset-before-current-sample-linear-or-nearest-wavetable-and-phase-continuity",
                "compiler-owned-exact-descriptor-kernel-plan-settings-immutable-resource-binding-buffer-producer-scope-and-zero-latency-authority",
                "allocation-free-prepared-callback-failure-atomic-state-upstream-schedule-retry-and-transport-reset",
                "exact-reference-band-limiting-negative-frequency-file-backed-and-metadata-wavetables-additional-interpolation-phase-sync-typed-trigger-multichannel-polyphony-and-proprietary-semantics-unsupported",
            },
        "oscillator should expose its exact promoted limitation IDs");

    const auto& filter = requireNodeType(registry, "talas.filter");
    require(
        filter.capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && filter.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel,
        "filter should use the shared kernel in both execution contexts");

    const auto& adsr = requireNodeType(registry, "talas.adsr");
    require(
        adsr.capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && adsr.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel
            && adsr.capabilities.typedEvents
            && !adsr.capabilities.transportAware,
        "ADSR should use the shared kernel and its proven direct typed-gate path in both execution contexts");

    for (const auto* migrated : {&oscillator, &filter, &adsr})
    {
        require(
            migrated->capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && migrated->capabilities.supportsReset
                && migrated->capabilities.latency.kind
                    == talas::core::LatencyKind::None,
            migrated->typeId
                + " should expose its proven descriptor-driven transport reset");
    }

    for (const auto typeId : {
             std::string_view{"talas.feedbackLoopInput"},
             std::string_view{"talas.feedbackLoopOutput"},
         })
    {
        const auto& feedback =
            requireNodeType(registry, std::string(typeId));
        require(
            feedback.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && feedback.capabilities.supportsReset
                && feedback.capabilities.latency.kind
                    == talas::core::LatencyKind::None,
            std::string(typeId)
                + " should expose shared pair state, transport reset, and no feed-forward latency");
    }

    constexpr std::array nativeUtilityIds = {
        std::string_view{"talas.clock"},
        std::string_view{"talas.clockDivider"},
        std::string_view{"talas.quantizer"},
        std::string_view{"talas.gate"},
        std::string_view{"talas.logic"},
        std::string_view{"talas.slew"},
    };
    for (const auto typeId : nativeUtilityIds)
    {
        const auto& utility = requireNodeType(registry, std::string(typeId));
        require(
            utility.supportLevel == talas::core::NodeSupportLevel::Utility
                && utility.capabilities.offline
                    == talas::core::ExecutionSupport::Supported
                && utility.capabilities.preparedRealtime
                    == (typeId == "talas.clock"
                            || typeId == "talas.clockDivider"
                            || typeId == "talas.logic"
                            || typeId == "talas.quantizer"
                            || typeId == "talas.gate" || typeId == "talas.slew"
                            ? talas::core::ExecutionSupport::Supported
                            : talas::core::ExecutionSupport::Subset)
                && utility.capabilities.offlineBinding
                    == talas::core::ExecutionBinding::Kernel
                && utility.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::Kernel,
            std::string(typeId)
                + " should expose its exact native offline/prepared execution level");
        require(
            !utility.capabilities.typedEvents
                && utility.capabilities.transportAware
                    == (typeId == "talas.clock")
                && utility.capabilities.supportsReset
                    == (typeId == "talas.clock"
                        || typeId == "talas.clockDivider"
                        || typeId == "talas.gate"
                        || typeId == "talas.slew")
                && utility.capabilities.latency.kind
                    == talas::core::LatencyKind::None,
            std::string(typeId)
                + " should expose the narrow proven event, transport, reset, and latency contract");
    }
    const std::vector<std::string> exactDividerLimitations{
        "offline-and-prepared-shared-checked-finite-positive-uint32-clock-divider",
        "static-or-direct-event-value-positive-integer-division-and-steps-with-static-scalar-or-connected-reset",
        "rising-edge-count-modulo-reduction-held-step-divided-gate-and-reset-precedence",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-input-producer-source-buffer-dual-output-scope-state-reset-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-modulus-schedule-state-output-retry-reset-discontinuity-and-zero-frame-no-op",
        "typed-event-clock-reset-dynamic-reset-fallback-fractional-counter-multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        requireNodeType(registry, "talas.clockDivider")
                .capabilities.limitations
            == exactDividerLimitations,
        "Clock Divider should publish its exact promoted limitation authority");
    const std::vector<std::string> exactClockLimitations{
        "offline-and-prepared-shared-checked-finite-long-double-phase-continuous-clock",
        "static-or-direct-event-value-positive-hertz-frequency-unit-duty-cycle-and-static-enabled",
        "exact-absolute-frame-and-epoch-rebase-phase-less-than-duty-zero-or-one-gate-and-disabled-zero-output",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-output-buffer-scope-state-reset-transport-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-reset-discontinuity-and-zero-frame-no-op",
        "external-sync-reset-input-phase-offset-alternate-pulse-shape-multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        requireNodeType(registry, "talas.clock").capabilities.limitations
            == exactClockLimitations,
        "Clock should publish its exact promoted limitation authority");
    const auto& logicLimitations =
        requireNodeType(registry, "talas.logic").capabilities.limitations;
    require(
        logicLimitations.size() == 6
            && logicLimitations.front()
                == "offline-and-prepared-shared-checked-finite-clean-room-static-choice-logic",
        "Logic should publish its exact promoted limitation authority");
    const auto& quantizerLimitations =
        requireNodeType(registry, "talas.quantizer").capabilities.limitations;
    require(
        quantizerLimitations.size() == 6
            && quantizerLimitations.front()
                == "offline-and-prepared-shared-checked-finite-clean-room-step-quantizer",
        "Step Quantizer should publish its exact promoted limitation authority");
    const auto& gateLimitations =
        requireNodeType(registry, "talas.gate").capabilities.limitations;
    require(
        gateLimitations.size() == 6
            && gateLimitations.front()
                == "offline-and-prepared-shared-checked-finite-binary64-schmitt-gate",
        "Gate should publish its exact promoted limitation authority");
    const auto& slewLimitations =
        requireNodeType(registry, "talas.slew").capabilities.limitations;
    const std::vector<std::string> exactSlewLimitations{
        "offline-and-prepared-shared-checked-finite-binary64-directional-exponential-slew",
        "static-or-direct-event-value-finite-nonnegative-rise-and-fall-seconds",
        "exact-first-sample-initialization-target-greater-or-equal-rise-selection-and-zero-time-direct-tracking",
        "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-state-reset-zero-latency-and-local-transaction-authority",
        "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-reset-and-zero-frame-no-op",
        "alternate-curve-rate-initialization-overshoot-multichannel-and-exact-reference-semantics-unsupported",
    };
    require(
        slewLimitations == exactSlewLimitations,
        "Slew should publish its exact promoted limitation authority");
    for (const auto typeId : {
             std::string_view{"talas.crossfade"},
             std::string_view{"talas.pan"},
             std::string_view{"talas.horizontalAmbisonicEncoder"},
             std::string_view{"talas.horizontalAmbisonicDecoder"},
             std::string_view{"talas.quantizer"},
             std::string_view{"talas.logic"},
             std::string_view{"talas.stereoMix2"},
             std::string_view{"talas.stereoMix4"},
             std::string_view{"talas.stereoInOutput4"},
             std::string_view{"talas.stereoInOutput8"},
         })
    {
        require(
            requireNodeType(registry, std::string(typeId)).capabilities.state
                == talas::core::NodeStateModel::Stateless,
            std::string(typeId) + " should expose a stateless kernel contract");
    }
    for (const auto typeId : {
             std::string_view{"talas.clock"},
             std::string_view{"talas.clockDivider"},
             std::string_view{"talas.gate"},
             std::string_view{"talas.slew"},
         })
    {
        require(
            requireNodeType(registry, std::string(typeId)).capabilities.state
                == talas::core::NodeStateModel::SessionStateful,
            std::string(typeId) + " should expose a session-stateful kernel contract");
    }
    const auto& audioInput = requireNodeType(registry, "talas.audioInput");
    require(
        audioInput.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && !audioInput.capabilities.supportsReset
            && audioInput.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "audio input should expose its session cursor without claiming reset or latency");
    for (const auto typeId : {
             std::string_view{"talas.audioOutput"},
             std::string_view{"talas.mixer"},
             std::string_view{"talas.sqrtMagnitude"},
             std::string_view{"talas.vca"},
             std::string_view{"talas.matrix4"},
             std::string_view{"talas.output4"},
             std::string_view{"talas.output6"},
         })
    {
        const auto& stateless =
            requireNodeType(registry, std::string(typeId));
        require(
            stateless.capabilities.state
                    == talas::core::NodeStateModel::Stateless
                && !stateless.capabilities.supportsReset
                && stateless.capabilities.latency.kind
                    == talas::core::LatencyKind::None,
            std::string(typeId)
                + " should retain a stateless zero-latency processing contract");
    }

    const auto& diskPlayer = requireNodeType(registry, "talas.diskPlayer");
    require(
        diskPlayer.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && diskPlayer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && diskPlayer.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && diskPlayer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && diskPlayer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::StreamRead,
        "disk-player should publish its bounded prepared stream-read subset");

    const auto& partialTrackPlayer = requireNodeType(
        registry, "talas.partialTrackPlayerV1");
    require(
        partialTrackPlayer.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && partialTrackPlayer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && partialTrackPlayer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead,
        "partial-track player should publish exact immutable offline and bounded realtime authority");

    const auto& diskCache = requireNodeType(registry, "talas.diskCache");
    require(
        diskCache.supportLevel
                == talas::core::NodeSupportLevel::FunctionalSubset
            && diskCache.supportSummary.find("talas.diskCache.capture.v1")
                != std::string::npos
            && diskCache.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && diskCache.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && diskCache.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && diskCache.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && diskCache.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::StreamRead
            && diskCache.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && diskCache.capabilities.supportsReset,
        "disk-cache should publish exact capture/playback functional-subset authority");
}

void testUnclassifiedSecondaryCapabilitiesRemainExplicitlyUnknown()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& diskPlayer = requireNodeType(registry, "talas.diskPlayer");
    require(
        talas::core::latencyKindName(diskPlayer.capabilities.latency.kind)
            == "variableOrUnknown",
        "unclassified disk-player latency must not be published as zero");

    // A Sound's own timing is classified: it is not latency (ADR-0011).
    const auto& timeOffset = requireNodeType(registry, "talas.timeOffset");
    require(
        talas::core::latencyKindName(timeOffset.capabilities.latency.kind)
            == "none",
        "a Time Offset's timing should not be published as latency");
}

void testM3SignalBusResourceAndLatencyContractsAreTyped()
{
    const talas::core::PortDescriptor audio{
        "audio",
        talas::core::PortType::Audio,
        true};
    require(
        audio.signal.rate == talas::core::PortSignalRate::Sample
            && audio.signal.audioLayout
                == talas::core::AudioBusLayoutKind::Mono
            && audio.signal.timelineBearing
            && audio.signal.spectralFormat
                == talas::core::SpectralFrameFormat::NotApplicable,
        "Audio ports should default to a mono sample-timeline contract");

    const talas::core::PortDescriptor partial{
        "partials",
        talas::core::PortType::Spectrum,
        false};
    require(
        partial.signal.rate == talas::core::PortSignalRate::Frame
            && partial.signal.timelineBearing
            && partial.signal.spectralFormat
                == talas::core::SpectralFrameFormat::LegacyPartialTriples,
        "existing Spectrum ports should remain typed partial-frame compatibility ports");

    const talas::core::PortDescriptor complex{
        "frames",
        talas::core::PortType::ComplexSpectrum,
        false};
    require(
        complex.signal.rate == talas::core::PortSignalRate::Frame
            && complex.signal.audioLayout
                == talas::core::AudioBusLayoutKind::Mono
            && complex.signal.timelineBearing
            && complex.signal.spectralFormat
                == talas::core::SpectralFrameFormat::ComplexSpectrumV1,
        "ComplexSpectrum ports should have an explicit frame/timeline contract");

    talas::core::NodeTypeDescriptor descriptor{
        "talas.contractFixture",
        "Contract Fixture",
        "Testing",
        {
            {"left", talas::core::PortType::Audio, true},
            {"right", talas::core::PortType::Audio, true},
        },
        {{"output", talas::core::PortType::Audio, false}},
        {
            {"length", "64", talas::expr::ValueDomain::Samples},
            talas::core::resourceParameter("sample", {}),
        },
    };
    descriptor.capabilities.latency = {
        talas::core::LatencyKind::PreparationTime,
        0,
        "length",
        talas::core::PreparedLatencyRule::ParameterFramesMinusOne,
    };
    descriptor.capabilities.outputBusLayouts = {
        talas::core::AudioBusLayoutKind::Stereo,
        talas::core::AudioBusLayoutKind::Discrete4,
    };
    talas::core::NodeRegistry registry;
    registry.addNodeType(std::move(descriptor));
    const auto& stored = requireNodeType(registry, "talas.contractFixture");
    require(
        stored.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && stored.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && stored.capabilities.latency.kind
                == talas::core::LatencyKind::PreparationTime
            && stored.capabilities.latency.parameterId == "length"
            && stored.capabilities.latency.preparedRule
                == talas::core::PreparedLatencyRule::ParameterFramesMinusOne,
        "resource, fan-in, and preparation-time latency axes should remain explicit");

    auto invalidComplex = complex;
    invalidComplex.signal.rate = talas::core::PortSignalRate::Sample;
    talas::core::NodeRegistry invalidRegistry;
    bool rejected = false;
    try
    {
        invalidRegistry.addNodeType({
            "talas.invalidComplex",
            "Invalid Complex",
            "Testing",
            {},
            {invalidComplex},
            {},
        });
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "ComplexSpectrum execution contracts must reject a non-frame rate");

    auto invalidAudio = audio;
    invalidAudio.signal.audioLayout =
        talas::core::AudioBusLayoutKind::NotApplicable;
    rejected = false;
    try
    {
        talas::core::NodeRegistry registryWithInvalidLayout;
        registryWithInvalidLayout.addNodeType({
            "talas.invalidAudioLayout",
            "Invalid Audio Layout",
            "Testing",
            {},
            {invalidAudio},
            {},
        });
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "timeline-bearing audio ports must reject an absent bus layout");

    auto invalidPreparedLatency = talas::core::NodeTypeDescriptor{
        "talas.invalidPreparedLatency",
        "Invalid Prepared Latency",
        "Testing",
        {},
        {},
        {{"length", "64", talas::expr::ValueDomain::Samples}},
    };
    invalidPreparedLatency.capabilities.latency = {
        talas::core::LatencyKind::PreparationTime,
        0,
        "length",
        talas::core::PreparedLatencyRule::None,
    };
    rejected = false;
    try
    {
        talas::core::NodeRegistry registryWithInvalidLatency;
        registryWithInvalidLatency.addNodeType(
            std::move(invalidPreparedLatency));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "preparation-time latency must reject an absent resolution rule");

    auto invalidResource = talas::core::NodeTypeDescriptor{
        "talas.invalidResource",
        "Invalid Resource",
        "Testing",
        {},
        {},
        {talas::core::resourceParameter("sample", {})},
    };
    invalidResource.capabilities.resourceAccess =
        talas::core::ResourceAccessMode::None;
    rejected = false;
    try
    {
        talas::core::NodeRegistry registryWithInvalidResource;
        registryWithInvalidResource.addNodeType(std::move(invalidResource));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "resource parameters must not publish a no-resource access contract");

    auto textResourceAuthority = talas::core::NodeTypeDescriptor{
        "talas.textResourceAuthority",
        "Text Resource Authority",
        "Testing",
        {},
        {},
        {talas::core::textParameter("samples", {})},
    };
    textResourceAuthority.capabilities.resourceAccess =
        talas::core::ResourceAccessMode::ImmutableRead;
    rejected = false;
    try
    {
        talas::core::NodeRegistry registryWithTextResourceAuthority;
        registryWithTextResourceAuthority.addNodeType(
            std::move(textResourceAuthority));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "ordinary text parameters must not grant resource authority");

    auto staticListResourceAuthority = talas::core::NodeTypeDescriptor{
        "talas.staticListResourceAuthority",
        "Static List Resource Authority",
        "Testing",
        {},
        {},
        {talas::core::staticResourceListParameter("samples", {})},
    };
    staticListResourceAuthority.capabilities.resourceAccess =
        talas::core::ResourceAccessMode::ImmutableRead;
    talas::core::NodeRegistry registryWithStaticListResourceAuthority;
    registryWithStaticListResourceAuthority.addNodeType(
        std::move(staticListResourceAuthority));

    auto missingResourceAuthority = talas::core::NodeTypeDescriptor{
        "talas.missingResourceAuthority",
        "Missing Resource Authority",
        "Testing",
        {},
        {},
        {{"value", "0", talas::expr::ValueDomain::Scalar}},
    };
    missingResourceAuthority.capabilities.resourceAccess =
        talas::core::ResourceAccessMode::ImmutableRead;
    rejected = false;
    try
    {
        talas::core::NodeRegistry registryWithoutResourceAuthority;
        registryWithoutResourceAuthority.addNodeType(
            std::move(missingResourceAuthority));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "resource access must retain an explicit typed parameter authority");

    const auto builtins = talas::core::makeBuiltinNodeRegistry();
    require(
        requireNodeType(builtins, "talas.audioOutput")
                .capabilities.outputBusLayouts
            == std::vector<talas::core::AudioBusLayoutKind>({
                talas::core::AudioBusLayoutKind::Mono,
                talas::core::AudioBusLayoutKind::Stereo,
            })
            && requireNodeType(builtins, "talas.output4")
                    .capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>({
                    talas::core::AudioBusLayoutKind::Discrete4,
                })
            && requireNodeType(builtins, "talas.output6")
                    .capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>({
                    talas::core::AudioBusLayoutKind::Discrete6,
                })
            && requireNodeType(builtins, "talas.matrix4")
                    .capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>({
                    talas::core::AudioBusLayoutKind::Discrete4,
                })
            && requireNodeType(builtins, "talas.stereoInOutput4")
                    .capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>({
                    talas::core::AudioBusLayoutKind::Discrete4,
                })
            && requireNodeType(builtins, "talas.stereoInOutput8")
                    .capabilities.outputBusLayouts
                == std::vector<talas::core::AudioBusLayoutKind>({
                    talas::core::AudioBusLayoutKind::Discrete8,
                }),
        "multichannel descriptors should publish their prepared bus-layout authority");
}

void testNativeComplexFftDescriptorsFreezePreparedContracts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& forward = requireNodeType(registry, "talas.forwardFftV1");
    const auto& inverse = requireNodeType(registry, "talas.inverseFftV1");

    require(forward.displayName == "Forward FFT v1"
                && inverse.displayName == "Inverse FFT v1"
                && forward.category == "Spectral Analysis FFT"
                && inverse.category == "Spectral Analysis FFT",
            "native FFT descriptors should retain their separately versioned identity");
    requirePorts(
        forward.inputs,
        {{"audioIn", talas::core::PortType::Audio, true}},
        forward.typeId,
        "input");
    requirePorts(
        forward.outputs,
        {{"spectrumOut", talas::core::PortType::ComplexSpectrum, false}},
        forward.typeId,
        "output");
    requirePorts(
        inverse.inputs,
        {{"spectrumIn", talas::core::PortType::ComplexSpectrum, true}},
        inverse.typeId,
        "input");
    requirePorts(
        inverse.outputs,
        {{"audioOut", talas::core::PortType::Audio, false}},
        inverse.typeId,
        "output");

    for (const auto* descriptor : {&forward, &inverse})
    {
        require(descriptor->supportLevel == talas::core::NodeSupportLevel::Utility,
                descriptor->typeId + " should remain a Talas utility");
        require(descriptor->parameters.size() == 3,
                descriptor->typeId + " should expose only prepared frame configuration");
        require(requireParameter(registry, descriptor->typeId, "frameSize").defaultExpression == "1024",
                descriptor->typeId + " frameSize should default to 1024");
        require(requireParameter(registry, descriptor->typeId, "hopSize").defaultExpression == "1024",
                descriptor->typeId + " hopSize should default to 1024");
        const auto& window = requireParameter(registry, descriptor->typeId, "window");
        require(window.defaultChoice == "rectangular"
                    && window.choices == std::vector<std::string>({"rectangular", "hann"}),
                descriptor->typeId + " should expose the frozen native window matrix");
        require(descriptor->capabilities.offline == talas::core::ExecutionSupport::Supported
                    && descriptor->capabilities.preparedRealtime == talas::core::ExecutionSupport::Supported
                    && descriptor->capabilities.offlineBinding == talas::core::ExecutionBinding::LegacyAdapter
                    && descriptor->capabilities.preparedRealtimeBinding == talas::core::ExecutionBinding::LegacyAdapter,
                descriptor->typeId + " should publish the shared offline/prepared implementation");
        require(descriptor->capabilities.state == talas::core::NodeStateModel::SessionStateful
                    && descriptor->capabilities.supportsReset
                    && descriptor->capabilities.transportAware
                    && !descriptor->capabilities.typedEvents
                    && descriptor->capabilities.resourceAccess == talas::core::ResourceAccessMode::None
                    && descriptor->capabilities.fanInLatencyPolicy == talas::core::FanInLatencyPolicy::NotApplicable,
                descriptor->typeId + " should expose exact state/reset/resource/fan-in authority");
    }

    require(forward.outputs.front().signal.spectralFormat
                == talas::core::SpectralFrameFormat::ComplexSpectrumV1
                && inverse.inputs.front().signal.spectralFormat
                    == talas::core::SpectralFrameFormat::ComplexSpectrumV1,
            "native FFT ports should carry only ComplexSpectrumV1 timelines");
    require(forward.capabilities.latency.kind == talas::core::LatencyKind::PreparationTime
                && forward.capabilities.latency.parameterId == "frameSize"
                && forward.capabilities.latency.preparedRule
                    == talas::core::PreparedLatencyRule::ParameterFramesMinusOne,
            "forward FFT should declare exact N-1 preparation-time latency");
    require(inverse.capabilities.latency.kind == talas::core::LatencyKind::None,
            "inverse FFT should add no intrinsic latency to an arrived complex frame");
}

void testHistoricalPackedFftDescriptorFreezesCompatibilityContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& fft = requireNodeType(registry, "talas.fft");
    require(fft.displayName == "FFT"
                && fft.category == "Spectral Analysis FFT",
            "historical FFT should retain its legacy identity");
    // The stereo format adds the right channel; projects before
    // format 62 keep the packed layout (ADR-0008 amendment).
    requirePorts(
        fft.inputs,
        {{"audioIn", talas::core::PortType::Audio, true}, {"rightIn", talas::core::PortType::Audio, false}},
        fft.typeId,
        "input");
    requirePorts(
        fft.outputs,
        {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
        fft.typeId,
        "output");
    require(fft.parameters.size() == 3,
            "FFT should declare length, inverse and packing");
    const auto& packing = requireParameter(registry, fft.typeId, "packing");
    require(packing.defaultChoice == "talasStereo"
                && packing.choices == std::vector<std::string>{"talasStereo", "legacyPacked"},
            "FFT should default to the stereo format");
    const auto& length = requireParameter(registry, fft.typeId, "length");
    const auto& inverse = requireParameter(registry, fft.typeId, "inverse");
    require(length.defaultExpression == "1024"
                && length.domain == talas::expr::ValueDomain::Scalar,
            "historical FFT length should default to a playable 1024");
    require(inverse.defaultBoolean == false,
            "historical FFT inverse should continue to default false");
    require(fft.supportLevel == talas::core::NodeSupportLevel::FunctionalSubset
                && fft.capabilities.offline == talas::core::ExecutionSupport::Supported
                && fft.capabilities.preparedRealtime == talas::core::ExecutionSupport::Supported
                && fft.capabilities.offlineBinding == talas::core::ExecutionBinding::LegacyAdapter
                && fft.capabilities.preparedRealtimeBinding == talas::core::ExecutionBinding::LegacyAdapter,
            "packed FFT should publish its fully supported frozen execution matrix without overclaiming maturity");
    require(fft.capabilities.state == talas::core::NodeStateModel::SessionStateful
                && fft.capabilities.supportsReset
                && fft.capabilities.transportAware
                && !fft.capabilities.typedEvents
                && fft.capabilities.resourceAccess == talas::core::ResourceAccessMode::None
                && fft.capabilities.fanInLatencyPolicy == talas::core::FanInLatencyPolicy::Align,
            "packed FFT should expose exact state/reset/transport authority");
    require(fft.capabilities.latency.kind == talas::core::LatencyKind::PreparationTime
                && fft.capabilities.latency.parameterId == "length"
                && fft.capabilities.latency.preparedRule
                    == talas::core::PreparedLatencyRule::ParameterFramesMinusOne,
            "each historical FFT adapter should declare exact N-1 latency");
}

void testLiveSpectralAnalysisDescriptorsFreezeNativePartialFrameBoundary()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& analysis = requireNodeType(
        registry,
        "talas.liveSpectralAnalysis");

    require(analysis.outputs.size() == 1
                && analysis.outputs.front().id == "spectrumOut"
                && analysis.outputs.front().type
                    == talas::core::PortType::Spectrum
                && analysis.outputs.front().signal.spectralFormat
                    == talas::core::SpectralFrameFormat::PartialSpectrumV1,
            "LiveSpectralAnalysis should publish bounded PartialSpectrumV1 frames");
    require(analysis.capabilities.offline
                    == talas::core::ExecutionSupport::Supported
                && analysis.capabilities.preparedRealtime
                    == talas::core::ExecutionSupport::Supported
                && analysis.capabilities.offlineBinding
                    == talas::core::ExecutionBinding::LegacyAdapter
                && analysis.capabilities.preparedRealtimeBinding
                    == talas::core::ExecutionBinding::LegacyAdapter,
            "LiveSpectralAnalysis should publish its shared prepared execution matrix");
    require(analysis.capabilities.state
                    == talas::core::NodeStateModel::SessionStateful
                && analysis.capabilities.supportsReset
                && analysis.capabilities.transportAware
                && !analysis.capabilities.typedEvents
                && analysis.capabilities.resourceAccess
                    == talas::core::ResourceAccessMode::None,
            "LiveSpectralAnalysis should publish exact state/reset/transport authority");
    require(analysis.capabilities.latency.kind
                    == talas::core::LatencyKind::PreparationTime
                && analysis.capabilities.latency.parameterId
                    == "lowestAnalyzedFreq"
                && analysis.capabilities.latency.preparedRule
                    == talas::core::PreparedLatencyRule::
                        SampleRateOverParameterNextPowerOfTwoMinusOne,
            "LiveSpectralAnalysis should publish derived N-1 preparation-time latency");

    const auto& partialTrackPlayer = requireNodeType(
        registry,
        "talas.partialTrackPlayerV1");
    require(std::ranges::find(
                partialTrackPlayer.capabilities.limitations,
                "prepared-realtime-maximum-128-tracks-per-segment")
                != partialTrackPlayer.capabilities.limitations.end(),
            "partial-track player should disclose its exact prepared realtime voice limit");
    require(std::ranges::find(
                partialTrackPlayer.capabilities.limitations,
                "maximum-4096-tracks-per-segment-for-offline-render")
                != partialTrackPlayer.capabilities.limitations.end(),
            "partial-track player should disclose its exact offline voice limit");

    const auto& legacyHold = requireNodeType(
        registry,
        "talas.partialSpectrumLegacyHoldV1");
    require(legacyHold.inputs.size() == 1
                && legacyHold.inputs.front().signal.spectralFormat
                    == talas::core::SpectralFrameFormat::PartialSpectrumV1
                && legacyHold.outputs.size() == 1
                && legacyHold.outputs.front().signal.spectralFormat
                    == talas::core::SpectralFrameFormat::LegacyPartialTriples,
            "partial-spectrum legacy hold should be the explicit native-to-legacy boundary");
    // ADR-0016 (2026-10-10): renderers split at each analysis frame, so
    // the Hold no longer collapses a block's frames.
    require(legacyHold.capabilities.limitations.empty(),
            "partial-spectrum legacy hold should no longer publish a timing collapse limitation");

    const auto& selector = requireNodeType(
        registry, "talas.spectrumTrackSelector");
    const auto legacyInput = std::ranges::find_if(
        selector.inputs, [](const auto& input) {
            return input.id == "spectrumIn";
        });
    const auto typedInput = std::ranges::find_if(
        selector.inputs, [](const auto& input) {
            return input.id == "partialSpectrumIn";
        });
    require(legacyInput != selector.inputs.end()
                && typedInput != selector.inputs.end()
                && !legacyInput->required && !typedInput->required
                && legacyInput->signal.spectralFormat
                    == talas::core::SpectralFrameFormat::LegacyPartialTriples
                && typedInput->signal.spectralFormat
                    == talas::core::SpectralFrameFormat::PartialSpectrumV1,
            "SpectrumTrackSelector should expose explicit legacy and timed typed inputs");
    const auto& fundamental = requireNodeType(
        registry, "talas.spectrumFundamental");
    require(fundamental.inputs.size() == 2
                && !fundamental.inputs[0].required
                && !fundamental.inputs[1].required
                && fundamental.inputs[0].signal.spectralFormat
                    == talas::core::SpectralFrameFormat::LegacyPartialTriples
                && fundamental.inputs[1].signal.spectralFormat
                    == talas::core::SpectralFrameFormat::PartialSpectrumV1,
            "SpectrumFundamental should expose explicit legacy and timed typed inputs");
    const auto& voiced = requireNodeType(
        registry, "talas.spectrumVoicedUnvoiced");
    require(voiced.inputs.size() == 2
                && !voiced.inputs[0].required
                && !voiced.inputs[1].required
                && voiced.inputs[0].signal.spectralFormat
                    == talas::core::SpectralFrameFormat::LegacyPartialTriples
                && voiced.inputs[1].signal.spectralFormat
                    == talas::core::SpectralFrameFormat::PartialSpectrumV1,
            "SpectrumVoicedUnvoiced should expose explicit legacy and timed typed inputs");
}

void testMaturityUpdatesDoNotRewritePreparedRealtimeCapability()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Test",
        "Testing",
        {},
        {},
        {},
    });
    registry.setNodeTypePreparedRealtimeSupport(
        "talas.oscillator",
        talas::core::ExecutionSupport::Subset,
        talas::core::ExecutionBinding::LegacyAdapter);
    registry.setNodeTypeSupport(
        "talas.oscillator",
        talas::core::NodeSupportLevel::FullRuntime,
        "Test fixture.");

    const auto& descriptor = requireNodeType(registry, "talas.oscillator");
    require(
        descriptor.capabilities.preparedRealtime
            == talas::core::ExecutionSupport::Subset,
        "maturity updates must preserve prepared-realtime support");
    require(
        std::ranges::find(
            descriptor.capabilities.limitations,
            "prepared-realtime-subset")
            != descriptor.capabilities.limitations.end(),
        "maturity updates must preserve prepared-realtime limitations");
    require(
        std::ranges::find(
            descriptor.capabilities.limitations,
            "prepared-realtime-unsupported")
            == descriptor.capabilities.limitations.end(),
        "maturity updates must not add contradictory realtime limitations");
}

void testCapabilityClaimsRequireImplementationBindings()
{
    talas::core::NodeTypeDescriptor descriptor;
    descriptor.typeId = "talas.invalidBinding";
    descriptor.displayName = "Invalid Binding";
    descriptor.category = "Testing";
    descriptor.capabilities.offline =
        talas::core::ExecutionSupport::Supported;
    descriptor.capabilities.offlineBinding =
        talas::core::ExecutionBinding::LegacyAdapter;

    talas::core::NodeRegistry registry;
    bool rejected = false;
    try
    {
        registry.addNodeType(std::move(descriptor));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(
        rejected,
        "execution claims without a registered implementation binding must be rejected");
}

void testRejectedCapabilityUpdateLeavesRegistryUnchanged()
{
    talas::core::NodeRegistry registry;
    registry.addNodeType({
        "talas.oscillator",
        "Oscillator",
        "Testing",
        {},
        {},
        {},
    });

    bool rejected = false;
    try
    {
        registry.setNodeTypeOfflineSupport(
            "talas.oscillator",
            talas::core::ExecutionSupport::Supported,
            talas::core::ExecutionBinding::None);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected, "invalid capability update should throw");

    const auto& descriptor =
        requireNodeType(registry, "talas.oscillator");
    require(
        descriptor.capabilities.offline
                == talas::core::ExecutionSupport::Unsupported
            && descriptor.capabilities.offlineBinding
                == talas::core::ExecutionBinding::None
            && descriptor.capabilities.limitations.empty(),
        "rejected capability update must not mutate stored metadata");
}

void testManualOnlyDescriptorsDeclareExactCatalogContracts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<ExpectedNodeDescriptor> expectedDescriptors = {
        {
            "talas.noise",
            "Noise",
            "Xtra Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"centerValue", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"initialState", "0.0", talas::expr::ValueDomain::Scalar},
                {"type", {}, talas::expr::ValueDomain::Scalar, "white", {"white", "pink", "hotPink"}},
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"centerValue", "0.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"hotPink", {}, talas::expr::ValueDomain::Scalar, "peakAtFrequency", {"peakAtFrequency", "legacySmoothedSteps", "occasionalJumps"}},
            },
        },
        {
            "talas.crossfade",
            "Crossfade",
            "Level, Compression, Expansion",
            {
                {"snd1", talas::core::PortType::Audio, true},
                {"snd2", talas::core::PortType::Audio, true},
                {"pan", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"fade", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"pan", "0.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"fade", "0.0", talas::expr::ValueDomain::Scalar},
                {"type", {}, talas::expr::ValueDomain::Scalar, "power", {"power", "linear"}},
            },
        },
        {
            "talas.dynamicRangeController",
            "Dynamic Range Controller",
            "Level, Compression, Expansion",
            {
                {"sideChain", talas::core::PortType::Audio, true},
                {"audioIn", talas::core::PortType::Audio, true},
                {"gain", talas::core::PortType::Control, false},
                {"threshold", talas::core::PortType::Control, false},
                {"ratio", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
                {"delay", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"type", {}, talas::expr::ValueDomain::Scalar, "compressor", {"compressor", "expander", "upwardExpander"}},
                {"ratio", "1.0", talas::expr::ValueDomain::Scalar},
                {"threshold", "0.5", talas::expr::ValueDomain::Scalar},
                {"attackTime", "10 ms", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "100 ms", talas::expr::ValueDomain::Seconds},
                {"delay", "0 s", talas::expr::ValueDomain::Seconds},
                {"gain", "1.0", talas::expr::ValueDomain::Scalar},
                {"ratioLaw", {}, talas::expr::ValueDomain::Scalar, "decibels", {"decibels", "legacyLinear"}},
            },
        },
        {
            "talas.level",
            "Level",
            "Level, Compression, Expansion",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"noGain", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"overflow", {}, talas::expr::ValueDomain::Scalar, "unclipped", {"unclipped", "clip"}},
            },
        },
        {
            "talas.multiplyingWaveshaper",
            "Multiplying Waveshaper",
            "Level, Compression, Expansion",
            {
                {"nonlinearInput", talas::core::PortType::Audio, true},
                {"audioIn", talas::core::PortType::Audio, true},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "FullRamp", {}, talas::core::ParameterKind::Resource},
            },
        },
        {
            "talas.sample",
            "Sample",
            "Sampling",
            {
                {"trigger", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
                {"start", talas::core::PortType::Control, false},
                {"end", talas::core::PortType::Control, false},
                {"loopStart", talas::core::PortType::Control, false},
                {"loopEnd", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "default", talas::expr::ValueDomain::Hertz},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"sample", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"setLoop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"loopStart", "0.0", talas::expr::ValueDomain::Scalar},
                {"loopEnd", "1.0", talas::expr::ValueDomain::Scalar},
                {"loopFade", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"start", "0.0", talas::expr::ValueDomain::Scalar},
                {"end", "1.0", talas::expr::ValueDomain::Scalar},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"attackTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"headerLoop", {}, talas::expr::ValueDomain::Scalar, "loop", {"loop", "legacyIgnore"}},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "legacyMono"}},
            },
        },
        {
            "talas.sampleMapPlayer",
            "Sample Map Player",
            "Sampling",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"velocity", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"sampleMapId", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"velocity", "1.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"attackTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "0 s", talas::expr::ValueDomain::Seconds},
            },
        },
        {
            "talas.forcedProcessorAssignment",
            "Forced Processor Assignment",
            "Sampling",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {{"processor", "1", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.diskCache",
            "Disk Cache",
            "Sampling",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"record", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.diskPlayer",
            "Disk Player",
            "Sampling",
            {
                {"filePosition", talas::core::PortType::Control, false},
                {"duration", talas::core::PortType::Control, false},
                {"trigger", talas::core::PortType::Control, false},
                {"rateScale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"filePosition", "0 s", talas::expr::ValueDomain::Seconds},
                {"duration", "1 s", talas::expr::ValueDomain::Seconds},
                {"rateScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"durationBounds", {}, talas::expr::ValueDomain::Scalar, "sound",
                    {"sound", "legacyPlayback"}},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "legacyMono"}},
            },
        },
        {
            "talas.partialTrackPlayerV1",
            "Partial Track Player",
            "Spectral",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"partialTrack", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"gain", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.diskRecorder",
            "Disk Recorder",
            "Sampling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"trigger", talas::core::PortType::Control, false},
                {"captureDuration", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"format", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"wordSize", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "mono", {"mono", "stereo"}},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"captureDuration", "0 s", talas::expr::ValueDomain::Seconds},
                {"gated", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.diskRecorderTap",
            "Disk Recorder Tap",
            "Sampling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"trigger", talas::core::PortType::Control, false},
                {"captureDuration", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"format", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"wordSize", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "mono", {"mono", "stereo"}},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"captureDuration", "0 s", talas::expr::ValueDomain::Seconds},
                {"gated", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.memoryWriter",
            "Memory Writer",
            "Sampling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"trigger", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"captureDuration", "0 s", talas::expr::ValueDomain::Seconds},
                {"global", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"cyclic", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"recordingName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"silent", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.keyMappedMultisample",
            "Key Mapped Multisample",
            "Sampling",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"loFreq", talas::core::PortType::Control, false},
                {"hiFreq", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"velocity", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"velocity", "1.0", talas::expr::ValueDomain::Scalar},
                {"firstSample", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"loFreq", "0 hz", talas::expr::ValueDomain::Hertz},
                {"hiFreq", "20000 hz", talas::expr::ValueDomain::Hertz},
                {"mapping", {}, talas::expr::ValueDomain::Scalar, "byBasePitch", {"onePerHalfStep", "equallySpaced", "byBasePitch", "byPitchRange"}},
                {"attackTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"loop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"fromDisk", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"noTransposition", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"headerLoop", {}, talas::expr::ValueDomain::Scalar, "loop", {"loop", "legacyIgnore"}},
                {"samples", {}, talas::expr::ValueDomain::Scalar, "folder", {"folder", "legacyFirstSampleOnly"}},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "legacyMono"}},
                {"halfStepsFrom", {}, talas::expr::ValueDomain::Scalar, "loFreq", {"loFreq", "keyOne"}},
            },
        },
        {
            "talas.multifileDiskPlayer",
            "Multifile Disk Player",
            "Sampling",
            {
                {"index", talas::core::PortType::Control, false},
                {"trigger", talas::core::PortType::Control, false},
                {"rateScale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"fileNames", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"rateScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"gated", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"index", "0", talas::expr::ValueDomain::Scalar},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "legacyMono"}},
            },
        },
        {
            "talas.multisample",
            "Multisample",
            "Sampling",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"index", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "0 hz", talas::expr::ValueDomain::Hertz},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"samples", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"index", "0", talas::expr::ValueDomain::Scalar},
                {"loop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"loopStart", "0.0", talas::expr::ValueDomain::Scalar},
                {"loopEnd", "1.0", talas::expr::ValueDomain::Scalar},
                {"attackTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"headerLoop", {}, talas::expr::ValueDomain::Scalar, "loop", {"loop", "legacyIgnore"}},
                {"gateRelease", {}, talas::expr::ValueDomain::Scalar, "playsThrough", {"playsThrough", "legacyStops"}},
                {"channels", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "legacyMono"}},
            },
        },
        {
            "talas.sampleAndHold",
            "Sample And Hold",
            "Sampling",
            {
                {"value", talas::core::PortType::Control, true},
                {"holdTime", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"holdTime", "1 s", talas::expr::ValueDomain::Seconds},
                {"offsetTime", "0 s", talas::expr::ValueDomain::Seconds},
            },
        },
        {
            "talas.sampleCloud",
            "Sample Cloud",
            "Sampling",
            {
                {"amplitude", talas::core::PortType::Control, false},
                {"pan", talas::core::PortType::Control, false},
                {"timeIndex", talas::core::PortType::Control, false},
                {"grainDur", talas::core::PortType::Control, false},
                {"density", talas::core::PortType::Control, false},
                {"maxGrains", talas::core::PortType::Control, false},
                {"panJitter", talas::core::PortType::Control, false},
                {"grainDurJitter", talas::core::PortType::Control, false},
                {"timeIndexJitter", talas::core::PortType::Control, false},
            },
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"sample", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"grainEnv", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"maxGrains", "16", talas::expr::ValueDomain::Scalar},
                {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
                {"density", "1.0", talas::expr::ValueDomain::Scalar},
                {"grainDur", "100 ms", talas::expr::ValueDomain::Seconds},
                {"grainDurJitter", "0.0", talas::expr::ValueDomain::Scalar},
                {"pan", "0.5", talas::expr::ValueDomain::Scalar},
                {"panJitter", "0.0", talas::expr::ValueDomain::Scalar},
                {"seed", "0.0", talas::expr::ValueDomain::Scalar},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"timeIndex", "0.0", talas::expr::ValueDomain::Scalar},
                {"timeIndexJitter", "0.0", talas::expr::ValueDomain::Scalar},
                {"grainShapes", {}, talas::expr::ValueDomain::Scalar, "atGrainStart",
                    {"atGrainStart", "legacyEveryFrame"}},
                {"grainEnvFromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.samplesFromDiskSingleStep",
            "Samples From Disk Single Step",
            "Sampling",
            {
                {"filePosition", talas::core::PortType::Control, false},
                {"trigger", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"filePosition", "0", talas::expr::ValueDomain::Scalar},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.spectralShape",
            "Spectral Shape",
            "Spectral Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"spacing", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"spectrumOut", talas::core::PortType::Spectrum, false},
                {"amplitudeOut", talas::core::PortType::Audio, false},
                {"frequencyOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"spacing", "440 hz", talas::expr::ValueDomain::Hertz},
                {"nbrPartials", "1", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"envelope", {}, talas::expr::ValueDomain::Scalar, "frequency",
                    {"frequency", "legacyPartial"}},
            },
        },
        {
            "talas.spectrumFrequencyScale",
            "Spectrum Frequency Scale",
            "Spectral Modifiers",
            {
                {"spectrumIn", talas::core::PortType::Spectrum, true},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"formantsOnly", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"formants", {}, talas::expr::ValueDomain::Scalar, "stay", {"stay", "legacyMove"}},
            },
        },
        {
            "talas.spectrumFundamental",
            "Spectrum Fundamental",
            "Spectral Modifiers",
            {
                {"spectrumIn", talas::core::PortType::Spectrum, false},
                {"partialSpectrumIn", talas::core::PortType::Spectrum, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.spectrumInRam",
            "Spectrum In RAM",
            "Spectral Sources",
            {
                {"level", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"timeIndex", talas::core::PortType::Control, false},
                {"nbrPartials", talas::core::PortType::Control, false},
                {"firstPartial", talas::core::PortType::Control, false},
            },
            {
                {"spectrumOut", talas::core::PortType::Spectrum, false},
                {"amplitudeOut", talas::core::PortType::Audio, false},
                {"frequencyOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "0 hz", talas::expr::ValueDomain::Hertz},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"timeIndex", "0.0", talas::expr::ValueDomain::Scalar},
                {"analysis", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "HarmonicSpectrum", {}, talas::core::ParameterKind::Resource},
                {"analysisFrames", "1", talas::expr::ValueDomain::Scalar},
                {"analysisFrequency", "1 hz", talas::expr::ValueDomain::Hertz},
                {"nbrPartials", "0", talas::expr::ValueDomain::Scalar},
                {"firstPartial", "1", talas::expr::ValueDomain::Scalar},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
            },
        },
        {
            "talas.spectrumLinearToLog",
            "Spectrum Linear To Log",
            "Spectral Modifiers",
            {{"spectrumIn", talas::core::PortType::Spectrum, true}},
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {},
        },
        {
            "talas.spectrumLogToLinear",
            "Spectrum Log To Linear",
            "Spectral Modifiers",
            {{"spectrumIn", talas::core::PortType::Spectrum, true}},
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {},
        },
        {
            "talas.spectrumModifier",
            "Spectrum Modifier",
            "Spectral Modifiers",
            {
                {"spectrumIn", talas::core::PortType::Spectrum, true},
                {"select", talas::core::PortType::Control, false},
                {"loTrack", talas::core::PortType::Control, false},
                {"hiTrack", talas::core::PortType::Control, false},
                {"loFreq", talas::core::PortType::Control, false},
                {"hiFreq", talas::core::PortType::Control, false},
                {"freqHysteresis", talas::core::PortType::Control, false},
                {"freqScale", talas::core::PortType::Control, false},
                {"freqOffset", talas::core::PortType::Control, false},
                {"loAmp", talas::core::PortType::Control, false},
                {"hiAmp", talas::core::PortType::Control, false},
                {"ampHysteresis", talas::core::PortType::Control, false},
                {"ampScale", talas::core::PortType::Control, false},
                {"ampOffset", talas::core::PortType::Control, false},
                {"probability", talas::core::PortType::Control, false},
                {"seed", talas::core::PortType::Control, false},
                {"hearAll", talas::core::PortType::Control, false},
                {"freqSmoothing", talas::core::PortType::Control, false},
                {"ampSmoothing", talas::core::PortType::Control, false},
            },
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {
                {"select", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"loTrack", "1", talas::expr::ValueDomain::Scalar},
                {"hiTrack", "10000", talas::expr::ValueDomain::Scalar},
                {"loFreq", "0 hz", talas::expr::ValueDomain::Hertz},
                {"hiFreq", "20000 hz", talas::expr::ValueDomain::Hertz},
                {"freqHysteresis", "0 hz", talas::expr::ValueDomain::Hertz},
                {"freqScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"freqOffset", "0.0", talas::expr::ValueDomain::Scalar},
                {"loAmp", "0.0", talas::expr::ValueDomain::Scalar},
                {"hiAmp", "1.0", talas::expr::ValueDomain::Scalar},
                {"ampHysteresis", "0.0", talas::expr::ValueDomain::Scalar},
                {"ampScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"ampOffset", "0.0", talas::expr::ValueDomain::Scalar},
                {"probability", "1.0", talas::expr::ValueDomain::Scalar},
                {"seed", "0.0", talas::expr::ValueDomain::Scalar},
                {"hearAll", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"freqSmoothing", "0.0", talas::expr::ValueDomain::Scalar},
                {"ampSmoothing", "0.0", talas::expr::ValueDomain::Scalar},
                {"freqOffsetUnits", {}, talas::expr::ValueDomain::Scalar, "fractionOfNyquist", {"fractionOfNyquist", "legacyHertz"}},
            },
        },
        {
            "talas.spectrumOnDisk",
            "Spectrum On Disk",
            "Spectral Sources",
            {
                {"level", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"rateScale", talas::core::PortType::Control, false},
                {"trigger", talas::core::PortType::Control, false},
                {"nbrPartials", talas::core::PortType::Control, false},
                {"firstPartial", talas::core::PortType::Control, false},
            },
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {
                {"frequency", "0 hz", talas::expr::ValueDomain::Hertz},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"rateScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"trigger", "0.0", talas::expr::ValueDomain::Scalar},
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "HarmonicSpectrum", {}, talas::core::ParameterKind::Resource},
                {"analysisFrames", "1", talas::expr::ValueDomain::Scalar},
                {"analysisFrequency", "1 hz", talas::expr::ValueDomain::Hertz},
                {"nbrPartials", "0", talas::expr::ValueDomain::Scalar},
                {"firstPartial", "1", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.spectrumTrackSelector",
            "Spectrum Track Selector",
            "Spectral Modifiers",
            {
                {"spectrumIn", talas::core::PortType::Spectrum, false},
                {"partialSpectrumIn", talas::core::PortType::Spectrum, false},
                {"track", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"track", "1", talas::expr::ValueDomain::Scalar},
                {"amp", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"freq", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"freqUnits", {}, talas::expr::ValueDomain::Scalar, "fractionOfNyquist",
                    {"fractionOfNyquist", "legacyHertz"}},
            },
        },
        {
            "talas.spectrumVoicedUnvoiced",
            "Spectrum Voiced Unvoiced",
            "Spectral Modifiers",
            {
                {"spectrumIn", talas::core::PortType::Spectrum, false},
                {"partialSpectrumIn", talas::core::PortType::Spectrum, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.syntheticSpectrumFromArray",
            "Synthetic Spectrum From Array",
            "Spectral Sources",
            {{"envelope", talas::core::PortType::Control, false}},
            {
                {"spectrumOut", talas::core::PortType::Spectrum, false},
                {"amplitudeOut", talas::core::PortType::Audio, false},
                {"frequencyOut", talas::core::PortType::Audio, false},
            },
            {
                {"nbrPartials", "4", talas::expr::ValueDomain::Scalar},
                {"logScale", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"sendBandwidths", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"envelope", "1.0", talas::expr::ValueDomain::Scalar},
                {"amplitudes", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0.5 0.25 0.125 0.0625", talas::core::ParameterKind::Text},
                {"frequencies", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "220 440 660 880", talas::core::ParameterKind::Text},
                {"bandwidths", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "50 50 50 50", talas::core::ParameterKind::Text},
                {"updates", {}, talas::expr::ValueDomain::Scalar, "everyNbrPartials", {"everyNbrPartials", "legacyEveryBlock"}},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
            },
        },
        {
            "talas.syntheticSpectrumFromSounds",
            "Synthetic Spectrum From Sounds",
            "Spectral Sources",
            {
                {"amplitudes", talas::core::PortType::Audio, true},
                {"frequenciesAndBandwidths", talas::core::PortType::Audio, true},
                {"nbrPartials", talas::core::PortType::Control, false},
            },
            {
                {"spectrumOut", talas::core::PortType::Spectrum, false},
                {"amplitudeOut", talas::core::PortType::Audio, false},
                {"frequencyOut", talas::core::PortType::Audio, false},
            },
            {
                {"nbrPartials", "1", talas::expr::ValueDomain::Scalar},
                {"logScale", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"includesBandwidths", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
                {"logFrequencies", {}, talas::expr::ValueDomain::Scalar, "pitch", {"pitch", "legacyLinear"}},
            },
        },
        {
            "talas.fft",
            "FFT",
            "Spectral Analysis FFT",
            {{"audioIn", talas::core::PortType::Audio, true}, {"rightIn", talas::core::PortType::Audio, false}},
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"length", "1024", talas::expr::ValueDomain::Scalar},
                {"inverse", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"packing", {}, talas::expr::ValueDomain::Scalar, "talasStereo", {"talasStereo", "legacyPacked"}},
            },
        },
        {
            "talas.feedbackLoopInput",
            "Feedback Loop Input",
            "Xtra",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {},
            {{"connection", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text}},
        },
        {
            "talas.feedbackLoopOutput",
            "Feedback Loop Output",
            "Xtra",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"connection", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"delay", "0 s", talas::expr::ValueDomain::Seconds},
            },
        },
        {
            "talas.horizontalAmbisonicEncoder",
            "Horizontal Ambisonic Encoder",
            "Spatializing",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"azimuth", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"wOut", talas::core::PortType::Audio, false},
                {"xOut", talas::core::PortType::Audio, false},
                {"yOut", talas::core::PortType::Audio, false},
            },
            {
                {"azimuth", "0.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.horizontalAmbisonicDecoder",
            "Horizontal Ambisonic Decoder",
            "Spatializing",
            {
                {"wIn", talas::core::PortType::Audio, true},
                {"xIn", talas::core::PortType::Audio, true},
                {"yIn", talas::core::PortType::Audio, true},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"out1", talas::core::PortType::Audio, false},
                {"out2", talas::core::PortType::Audio, false},
                {"out3", talas::core::PortType::Audio, false},
                {"out4", talas::core::PortType::Audio, false},
                {"out5", talas::core::PortType::Audio, false},
                {"out6", talas::core::PortType::Audio, false},
                {"out7", talas::core::PortType::Audio, false},
                {"out8", talas::core::PortType::Audio, false},
            },
            {
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"layout", {}, talas::expr::ValueDomain::Scalar, "discrete4", {"discrete4", "discrete8"}},
            },
        },
        {
            "talas.matrix4",
            "Matrix 4",
            "Spatializing",
            {
                {"in1", talas::core::PortType::Audio, true},
                {"in2", talas::core::PortType::Audio, true},
                {"in3", talas::core::PortType::Audio, true},
                {"in4", talas::core::PortType::Audio, true},
            },
            {
                {"out1", talas::core::PortType::Audio, false},
                {"out2", talas::core::PortType::Audio, false},
                {"out3", talas::core::PortType::Audio, false},
                {"out4", talas::core::PortType::Audio, false},
            },
            {
                {"insToOut1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "1 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut2", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 1 0 0", talas::core::ParameterKind::Text},
                {"insToOut3", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 1 0", talas::core::ParameterKind::Text},
                {"insToOut4", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 1", talas::core::ParameterKind::Text},
            },
        },
        {
            "talas.matrix8",
            "Matrix 8",
            "Spatializing",
            {
                {"in1", talas::core::PortType::Audio, true},
                {"in2", talas::core::PortType::Audio, true},
                {"in3", talas::core::PortType::Audio, true},
                {"in4", talas::core::PortType::Audio, true},
                {"in5", talas::core::PortType::Audio, true},
                {"in6", talas::core::PortType::Audio, true},
                {"in7", talas::core::PortType::Audio, true},
                {"in8", talas::core::PortType::Audio, true},
            },
            {
                {"out1", talas::core::PortType::Audio, false},
                {"out2", talas::core::PortType::Audio, false},
                {"out3", talas::core::PortType::Audio, false},
                {"out4", talas::core::PortType::Audio, false},
                {"out5", talas::core::PortType::Audio, false},
                {"out6", talas::core::PortType::Audio, false},
                {"out7", talas::core::PortType::Audio, false},
                {"out8", talas::core::PortType::Audio, false},
            },
            {
                {"insToOut1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "1 0 0 0 0 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut2", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 1 0 0 0 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut3", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 1 0 0 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut4", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 1 0 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut5", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 0 1 0 0 0", talas::core::ParameterKind::Text},
                {"insToOut6", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 0 0 1 0 0", talas::core::ParameterKind::Text},
                {"insToOut7", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 0 0 0 1 0", talas::core::ParameterKind::Text},
                {"insToOut8", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 0 0 0 0 0 0 1", talas::core::ParameterKind::Text},
            },
        },
        {
            "talas.output4",
            "Output 4",
            "Spatializing",
            {
                {"out1", talas::core::PortType::Audio, true},
                {"out2", talas::core::PortType::Audio, true},
                {"out3", talas::core::PortType::Audio, true},
                {"out4", talas::core::PortType::Audio, true},
            },
            {},
            {},
        },
        {
            "talas.output6",
            "Output 6",
            "Spatializing",
            {
                {"out1", talas::core::PortType::Audio, true},
                {"out2", talas::core::PortType::Audio, true},
                {"out3", talas::core::PortType::Audio, true},
                {"out4", talas::core::PortType::Audio, true},
                {"out5", talas::core::PortType::Audio, true},
                {"out6", talas::core::PortType::Audio, true},
            },
            {},
            {},
        },
        {
            "talas.output8",
            "Output 8",
            "Spatializing",
            {
                {"out1", talas::core::PortType::Audio, true},
                {"out2", talas::core::PortType::Audio, true},
                {"out3", talas::core::PortType::Audio, true},
                {"out4", talas::core::PortType::Audio, true},
                {"out5", talas::core::PortType::Audio, true},
                {"out6", talas::core::PortType::Audio, true},
                {"out7", talas::core::PortType::Audio, true},
                {"out8", talas::core::PortType::Audio, true},
            },
            {},
            {},
        },
        {
            "talas.stereoInOutput4",
            "Stereo In Output 4",
            "Spatializing",
            {
                {"out12", talas::core::PortType::Audio, true},
                {"out34", talas::core::PortType::Audio, true},
                {"out12Right", talas::core::PortType::Audio, false},
                {"out34Right", talas::core::PortType::Audio, false},
            },
            {},
            {},
        },
        {
            "talas.stereoInOutput8",
            "Stereo In Output 8",
            "Spatializing",
            {
                {"out12", talas::core::PortType::Audio, true},
                {"out34", talas::core::PortType::Audio, true},
                {"out56", talas::core::PortType::Audio, true},
                {"out78", talas::core::PortType::Audio, true},
                {"out12Right", talas::core::PortType::Audio, false},
                {"out34Right", talas::core::PortType::Audio, false},
                {"out56Right", talas::core::PortType::Audio, false},
                {"out78Right", talas::core::PortType::Audio, false},
            },
            {},
            {},
        },
        {
            "talas.midiFileEcho",
            "MIDI File Echo",
            "MIDI Out",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"lowChannel", "1", talas::expr::ValueDomain::Scalar},
                {"highChannel", "16", talas::expr::ValueDomain::Scalar},
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
            },
        },
        {
            "talas.midiFileEchoTap",
            "MIDI File Echo Tap",
            "MIDI Out",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"lowChannel", "1", talas::expr::ValueDomain::Scalar},
                {"highChannel", "16", talas::expr::ValueDomain::Scalar},
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
            },
        },
        {
            "talas.midiMapper",
            "MIDI Mapper",
            "MIDI In",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"map", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"voiceModel", {}, talas::expr::ValueDomain::Scalar,
                    "polyphonicInput", {"legacyPassthrough", "polyphonicInput"}},
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"channel", "0", talas::expr::ValueDomain::Scalar},
                {"source", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"midiFile", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"polyphony", "4", talas::expr::ValueDomain::Scalar},
                {"lowPitch", "0 nn", talas::expr::ValueDomain::NoteNumber},
                {"highPitch", "127 nn", talas::expr::ValueDomain::NoteNumber},
                {"script", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"shared", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"unsharedNotes", {}, talas::expr::ValueDomain::Scalar, "toFirstSound", {"toFirstSound", "legacyToEach"}},
                {"constantFieldsRead", {}, talas::expr::ValueDomain::Scalar, "eachVoice", {"eachVoice", "legacyWhenCompiled"}},
                {"scriptNotePitch", {}, talas::expr::ValueDomain::Scalar, "lowPitch", {"lowPitch", "legacyNoteZero"}},
            },
        },
        {
            "talas.midiOutputController",
            "MIDI Output Controller",
            "MIDI Out",
            {
                {"channel", talas::core::PortType::Control, false},
                {"controllerNumber", talas::core::PortType::Control, false},
                {"value", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"channel", "1", talas::expr::ValueDomain::Scalar},
                {"controllerNumber", "7", talas::expr::ValueDomain::Scalar},
                {"value", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "!Amp", talas::core::ParameterKind::Text},
                {"valueMeans", {}, talas::expr::ValueDomain::Scalar, "amplitude",
                    {"amplitude", "legacyControllerValue"}},
            },
        },
        {
            "talas.midiOutputControllerTap",
            "MIDI Output Controller Tap",
            "MIDI Out",
            {
                {"channel", talas::core::PortType::Control, false},
                {"controllerNumber", talas::core::PortType::Control, false},
                {"value", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"channel", "1", talas::expr::ValueDomain::Scalar},
                {"controllerNumber", "7", talas::expr::ValueDomain::Scalar},
                {"value", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "!Amp", talas::core::ParameterKind::Text},
                {"valueMeans", {}, talas::expr::ValueDomain::Scalar, "amplitude",
                    {"amplitude", "legacyControllerValue"}},
            },
        },
        {
            "talas.midiOutputEvent",
            "MIDI Output Event",
            "MIDI Out",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"amplitude", talas::core::PortType::Control, false},
                {"channel", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
                {"channel", "1", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"offKeyPitch", {}, talas::expr::ValueDomain::Scalar, "pitchBend",
                    {"pitchBend", "legacyNearestNote"}},
            },
        },
        {
            "talas.midiOutputEventTap",
            "MIDI Output Event Tap",
            "MIDI Out",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"amplitude", talas::core::PortType::Control, false},
                {"channel", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
                {"channel", "1", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"offKeyPitch", {}, talas::expr::ValueDomain::Scalar, "pitchBend",
                    {"pitchBend", "legacyNearestNote"}},
            },
        },
        {
            "talas.midiOutputEventInBytes",
            "MIDI Output Event In Bytes",
            "MIDI Out",
            {},
            {},
            {{"bytes", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text}},
        },
        {
            "talas.midiVoice",
            "MIDI Voice",
            "MIDI In",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"channel", "0", talas::expr::ValueDomain::Scalar},
                {"source", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"midiFile", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"polyphony", "2", talas::expr::ValueDomain::Scalar},
                {"lowPitch", "0 nn", talas::expr::ValueDomain::NoteNumber},
                {"highPitch", "127 nn", talas::expr::ValueDomain::NoteNumber},
                {"script", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"shared", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"unsharedNotes", {}, talas::expr::ValueDomain::Scalar, "toFirstSound", {"toFirstSound", "legacyToEach"}},
                {"constantFieldsRead", {}, talas::expr::ValueDomain::Scalar, "eachVoice", {"eachVoice", "legacyWhenCompiled"}},
                {"scriptNotePitch", {}, talas::expr::ValueDomain::Scalar, "lowPitch", {"lowPitch", "legacyNoteZero"}},
            },
        },
        {
            "talas.cloudBank",
            "Cloud Bank",
            "Aggregate Synthesis",
            {
                {"trigger", talas::core::PortType::Control, true},
                {"pan", talas::core::PortType::Control, true},
                {"freqMod", talas::core::PortType::Control, true},
                {"grainDurMod", talas::core::PortType::Control, true},
                {"spectrumSource", talas::core::PortType::Spectrum, false},
                {"amplitudeIn", talas::core::PortType::Audio, false},
                {"frequencyIn", talas::core::PortType::Audio, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"nbrTracks", "2", talas::expr::ValueDomain::Scalar},
                {"grainsPerTrack", "4", talas::expr::ValueDomain::Scalar},
                {"bankSize", "default", talas::expr::ValueDomain::Scalar},
                {"grainWave", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
                {"grainEnvelope", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"grainDur", "0.01 s", talas::expr::ValueDomain::Seconds},
                {"cyclesPerGrain", "1", talas::expr::ValueDomain::Scalar},
                {"seed", "0", talas::expr::ValueDomain::Scalar},
                {"interpolation", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "none"}},
                {"grainStarts", {}, talas::expr::ValueDomain::Scalar, "trigger", {"trigger", "legacyRegular"}},
            },
        },
        {
            "talas.cloudBankElement",
            "Cloud Bank Element",
            "Xtra Sources",
            {
                {"trigger", talas::core::PortType::Control, true},
                {"pan", talas::core::PortType::Control, true},
                {"freqMod", talas::core::PortType::Control, true},
                {"grainDurMod", talas::core::PortType::Control, true},
                {"spectrumSource", talas::core::PortType::Spectrum, false},
                {"cascadeInput", talas::core::PortType::Audio, true},
                {"amplitudeIn", talas::core::PortType::Audio, false},
                {"frequencyIn", talas::core::PortType::Audio, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"first", "1", talas::expr::ValueDomain::Scalar},
                {"count", "2", talas::expr::ValueDomain::Scalar},
                {"grainWave", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
                {"grainEnvelope", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"grainDur", "0.01 s", talas::expr::ValueDomain::Seconds},
                {"cyclesPerGrain", "1", talas::expr::ValueDomain::Scalar},
                {"maxGrains", "4", talas::expr::ValueDomain::Scalar},
                {"seed", "0", talas::expr::ValueDomain::Scalar},
                {"initialDelay", "0 s", talas::expr::ValueDomain::Seconds},
                {"interpolation", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "none"}},
                {"grainStarts", {}, talas::expr::ValueDomain::Scalar, "trigger", {"trigger", "legacyRegular"}},
            },
        },
        {
            "talas.filterBank",
            "Filter Bank",
            "Aggregate Synthesis",
            {
                {"spectrum", talas::core::PortType::Spectrum, true},
                {"audioIn", talas::core::PortType::Audio, true},
                {"bandwidth", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"nbrFilters", "8", talas::expr::ValueDomain::Scalar},
                {"bankSize", "8", talas::expr::ValueDomain::Scalar},
                {"bandwidth", "1.0", talas::expr::ValueDomain::Scalar},
                {"bandwidthUnits", {}, talas::expr::ValueDomain::Scalar, "spacing",
                    {"spacing", "legacyHertz"}},
            },
        },
        {
            "talas.averagingLowPassFilter",
            "Averaging Low Pass Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"cutoff", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"cutoff", "1000 hz", talas::expr::ValueDomain::Hertz},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Private", {}, talas::core::ParameterKind::Resource},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.dualParallelTwoPoleFilter",
            "Dual Parallel Two Pole Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"scale1", talas::core::PortType::Control, false},
                {"scale2", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"pole1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0.99495 % 0.031", talas::core::ParameterKind::Text},
                {"scale1", "0.3", talas::expr::ValueDomain::Scalar},
                {"pole2", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0.96775 % 0.22449", talas::core::ParameterKind::Text},
                {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.firFilter",
            "FIR Filter",
            "Filters",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {{"coefficients", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0.25 0.5 0.25", talas::core::ParameterKind::Text}},
        },
        {
            "talas.graphicEq",
            "Graphic EQ",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"cf250Hz", talas::core::PortType::Control, false},
                {"cf500Hz", talas::core::PortType::Control, false},
                {"cf1000Hz", talas::core::PortType::Control, false},
                {"cf2000Hz", talas::core::PortType::Control, false},
                {"cf4000Hz", talas::core::PortType::Control, false},
                {"cf8000Hz", talas::core::PortType::Control, false},
                {"cf16000Hz", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"cf250Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf500Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf1000Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf2000Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf4000Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf8000Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"cf16000Hz", "1.0", talas::expr::ValueDomain::Scalar},
                {"outerBands", {}, talas::expr::ValueDomain::Scalar, "shelving", {"shelving", "legacyBandPass"}},
            },
        },
        {
            "talas.harmonicResonator",
            "Harmonic Resonator",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"decayTime", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"decayTime", "1 s", talas::expr::ValueDomain::Seconds},
                {"brightness", "1.0", talas::expr::ValueDomain::Scalar},
                {"frequency", "220 hz", talas::expr::ValueDomain::Hertz},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Private", {}, talas::core::ParameterKind::Resource},
                {"prezero", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"scale", "0.1", talas::expr::ValueDomain::Scalar},
                {"tuning", {}, talas::expr::ValueDomain::Scalar, "exact", {"exact", "legacyRounded"}},
            },
        },
        {
            "talas.modalFilter",
            "Modal Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"frequencyScale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"modeAmps", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "#(1)", talas::core::ParameterKind::Text},
                {"modeFreqs", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "#(440)", talas::core::ParameterKind::Text},
                {"modeBWs", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "#(0.1)", talas::core::ParameterKind::Text},
                {"inputLevel", "1", talas::expr::ValueDomain::Scalar},
                {"envelope", "1", talas::expr::ValueDomain::Scalar},
                {"frequencyScale", "1", talas::expr::ValueDomain::Scalar},
                {"reset", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.highShelvingFilter",
            "High Shelving Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"cutoffFreq", talas::core::PortType::Control, false},
                {"boostOrCut", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"cutoffFreq", "2000 hz", talas::expr::ValueDomain::Hertz},
                {"boostOrCut", "6", talas::expr::ValueDomain::Scalar},
                {"scale", "0.5", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.lowShelvingFilter",
            "Low Shelving Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"cutoffFreq", talas::core::PortType::Control, false},
                {"boostOrCut", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"cutoffFreq", "200 hz", talas::expr::ValueDomain::Hertz},
                {"boostOrCut", "6", talas::expr::ValueDomain::Scalar},
                {"scale", "0.5", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.presenceFilter",
            "Presence Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"centerFreq", talas::core::PortType::Control, false},
                {"bandwidth", talas::core::PortType::Control, false},
                {"boostOrCut", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"centerFreq", "1000 hz", talas::expr::ValueDomain::Hertz},
                {"bandwidth", "200 hz", talas::expr::ValueDomain::Hertz},
                {"boostOrCut", "6", talas::expr::ValueDomain::Scalar},
                {"scale", "0.5", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.reResonator",
            "RE Resonator",
            "Filters",
            {{"audioIn", talas::core::PortType::Audio, true}, {"timeIndex", talas::core::PortType::Control, false}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Resonance", {}, talas::core::ParameterKind::Resource},
                {"timeIndex", "0.0", talas::expr::ValueDomain::Scalar},
                {"filterForm", {}, talas::expr::ValueDomain::Scalar, "allPoleLattice", {"allPoleLattice", "legacyFir"}},
            },
        },
        {
            "talas.twoFormantElement",
            "Two Formant Element",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"formant1", talas::core::PortType::Control, false},
                {"bandwidth1", talas::core::PortType::Control, false},
                {"scale1", talas::core::PortType::Control, false},
                {"formant2", talas::core::PortType::Control, false},
                {"bandwidth2", talas::core::PortType::Control, false},
                {"scale2", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"formant1", "238 hz", talas::expr::ValueDomain::Hertz},
                {"bandwidth1", "70 hz", talas::expr::ValueDomain::Hertz},
                {"scale1", "0.3", talas::expr::ValueDomain::Scalar},
                {"formant2", "1741 hz", talas::expr::ValueDomain::Hertz},
                {"bandwidth2", "100 hz", talas::expr::ValueDomain::Hertz},
                {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
                {"normalization", {}, talas::expr::ValueDomain::Scalar, "none", {"none", "unityPeak"}},
            },
        },
        {
            "talas.vcf",
            "VCF",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"amplitude", talas::core::PortType::Control, false},
                {"cutoff", talas::core::PortType::Control, false},
                {"cutoffModulator", talas::core::PortType::Control, false},
                {"cutoffModRange", talas::core::PortType::Control, false},
                {"resonance", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
                {"cutoff", "1000 hz", talas::expr::ValueDomain::Hertz},
                {"cutoffModulator", "0.0", talas::expr::ValueDomain::Scalar},
                {"cutoffModRange", "1000 hz", talas::expr::ValueDomain::Hertz},
                {"resonance", "0.5", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.scaleVocoder",
            "Scale Vocoder",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"sideChain", talas::core::PortType::Audio, true},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds},
                {"attackTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"nbrBands", "7", talas::expr::ValueDomain::Scalar},
                {"bankSize", "7", talas::expr::ValueDomain::Scalar},
                {"tonic", "4 c", talas::expr::ValueDomain::NoteNumber},
                {"intervals", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "0 2 4 5 7 9 11", talas::core::ParameterKind::Text},
                {"sideLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"inputLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"inBandwidth", "1.0", talas::expr::ValueDomain::Scalar},
                {"sideBandwidth", "1.0", talas::expr::ValueDomain::Scalar},
                {"tone", "0.0", talas::expr::ValueDomain::Scalar},
                {"rolloff", "0.5", talas::expr::ValueDomain::Scalar},
                {"gain", "1.0", talas::expr::ValueDomain::Scalar},
                {"rolloffShape", {}, talas::expr::ValueDomain::Scalar, "toneWidth", {"toneWidth", "legacyTilt"}},
            },
        },
        {
            "talas.tunableVocoder",
            "Tunable Vocoder",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"sideChain", talas::core::PortType::Audio, true},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds},
                {"attackTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"nbrBands", "16", talas::expr::ValueDomain::Scalar},
                {"bankSize", "16", talas::expr::ValueDomain::Scalar},
                {"logSpacing", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"analysisFreq", "100 hz", talas::expr::ValueDomain::Hertz},
                {"synthesisFreq", "100 hz", talas::expr::ValueDomain::Hertz},
                {"analysisLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"synthesisLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"analysisSpacing", "100", talas::expr::ValueDomain::Scalar},
                {"synthesisSpacing", "100", talas::expr::ValueDomain::Scalar},
                {"analysisBW", "1.0", talas::expr::ValueDomain::Scalar},
                {"synthesisBW", "1.0", talas::expr::ValueDomain::Scalar},
                {"tone", "0.0", talas::expr::ValueDomain::Scalar},
                {"rolloff", "0.5", talas::expr::ValueDomain::Scalar},
                {"gain", "1.0", talas::expr::ValueDomain::Scalar},
                {"spacingUnits", {}, talas::expr::ValueDomain::Scalar, "hzOrNn", {"hzOrNn", "legacyRelative"}},
                {"rolloffShape", {}, talas::expr::ValueDomain::Scalar, "toneWidth", {"toneWidth", "legacyTilt"}},
            },
        },
        {
            "talas.vocoder",
            "Vocoder",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"sideChain", talas::core::PortType::Audio, true},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"timeConstant", "0.05 s", talas::expr::ValueDomain::Seconds},
                {"attackTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"nbrBands", "16", talas::expr::ValueDomain::Scalar},
                {"bankSize", "16", talas::expr::ValueDomain::Scalar},
                {"inputLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"sideLevel", "1.0", talas::expr::ValueDomain::Scalar},
                {"lowCF", "100 hz", talas::expr::ValueDomain::Hertz},
                {"highCF", "5000 hz", talas::expr::ValueDomain::Hertz},
                {"inFreq", "1.0", talas::expr::ValueDomain::Scalar},
                {"sideFreq", "1.0", talas::expr::ValueDomain::Scalar},
                {"bw", "1.0", talas::expr::ValueDomain::Scalar},
                {"pitch", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"loCutoff", "50 hz", talas::expr::ValueDomain::Hertz},
                {"hiCutoff", "10000 hz", talas::expr::ValueDomain::Hertz},
                {"rolloff", "0.5", talas::expr::ValueDomain::Scalar},
                {"inputShaping", {}, talas::expr::ValueDomain::Scalar, "cutoffs", {"cutoffs", "legacyTilt"}},
            },
        },
        {
            "talas.vocoderChannelBank",
            "Vocoder Channel Bank",
            "Filters",
            {
                {"cascadeInput", talas::core::PortType::Audio, true},
                {"audioIn", talas::core::PortType::Audio, true},
                {"sideChain", talas::core::PortType::Audio, true},
                {"inputParameters", talas::core::PortType::Spectrum, true},
                {"sideChainParameters", talas::core::PortType::Spectrum, true},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"timeConstant", "5 ms", talas::expr::ValueDomain::Seconds},
                {"attackTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "-1 s", talas::expr::ValueDomain::Seconds},
                {"first", "1", talas::expr::ValueDomain::Scalar},
                {"count", "2", talas::expr::ValueDomain::Scalar},
                {"gain", {}, talas::expr::ValueDomain::Scalar, "nbrPartials", {"nbrPartials", "legacyCount"}},
            },
        },
        {
            "talas.filterBankElement",
            "Filter Bank Element",
            "Xtra Sources",
            {
                {"cascadeInput", talas::core::PortType::Audio, true},
                {"audioIn", talas::core::PortType::Audio, true},
                {"spectrum", talas::core::PortType::Spectrum, true},
                {"bandwidth", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"first", "1", talas::expr::ValueDomain::Scalar},
                {"count", "8", talas::expr::ValueDomain::Scalar},
                {"bandwidth", "1.0", talas::expr::ValueDomain::Scalar},
                {"bandwidthUnits", {}, talas::expr::ValueDomain::Scalar, "spacing",
                    {"spacing", "legacyHertz"}},
            },
        },
        {
            "talas.formantBank",
            "Formant Bank",
            "Aggregate Synthesis",
            {
                {"spectrum", talas::core::PortType::Spectrum, false},
                {"frequency", talas::core::PortType::Control, false},
                {"amplitudeIn", talas::core::PortType::Audio, false},
                {"frequencyIn", talas::core::PortType::Audio, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "110 hz", talas::expr::ValueDomain::Hertz},
                {"nbrFormants", "4", talas::expr::ValueDomain::Scalar},
                {"bankSize", "4", talas::expr::ValueDomain::Scalar},
                {"impulseResponse", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "LinearEnvelope", {}, talas::core::ParameterKind::Resource},
                {"nbrImpulses", "1", talas::expr::ValueDomain::Scalar},
                {"allowDC", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
            },
        },
        {
            "talas.formantBankOscillator",
            "Formant Bank Oscillator",
            "Xtra Sources",
            {
                {"spectrum", talas::core::PortType::Spectrum, false},
                {"cascadeInput", talas::core::PortType::Audio, true},
                {"frequency", talas::core::PortType::Control, false},
                {"amplitudeIn", talas::core::PortType::Audio, false},
                {"frequencyIn", talas::core::PortType::Audio, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "110 hz", talas::expr::ValueDomain::Hertz},
                {"firstFormant", "1", talas::expr::ValueDomain::Scalar},
                {"nbrFormants", "4", talas::expr::ValueDomain::Scalar},
                {"formantImpulse", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "LinearEnvelope", {}, talas::core::ParameterKind::Resource},
                {"nbrImpulses", "4", talas::expr::ValueDomain::Scalar},
                {"allowFormantAtDC", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
            },
        },
        {
            "talas.gaOscillators",
            "GA Oscillators",
            "Xtra Sources",
            {
                {"envelope", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"morph", talas::core::PortType::Control, false},
                {"timeIndex", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"timeIndex", "0.0", talas::expr::ValueDomain::Scalar},
                {"analysis0", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "HarmonicAmplitudes", {}, talas::core::ParameterKind::Resource},
                {"analysis1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"envelope", "1.0", talas::expr::ValueDomain::Scalar},
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"morph", "0.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.oscillatorBank",
            "Oscillator Bank",
            "Aggregate Synthesis",
            {
                {"spectrum", talas::core::PortType::Spectrum, false},
                {"amplitudeIn", talas::core::PortType::Audio, false},
                {"frequencyIn", talas::core::PortType::Audio, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"nbrOscillators", "8", talas::expr::ValueDomain::Scalar},
                {"bankSize", "8", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
                {"pairFrequencies", {}, talas::expr::ValueDomain::Scalar, "halfSampleRate", {"halfSampleRate", "legacyHertz"}},
            },
        },
        {
            "talas.genericSource",
            "Generic Source",
            "Xtra Sources",
            {
                {"trigger", talas::core::PortType::Control, false},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"source", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"leftChannel", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"rightChannel", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"sample", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"autoloop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"trigger", "1", talas::expr::ValueDomain::Scalar},
                {"attackTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"frequency", "4 c", talas::expr::ValueDomain::Hertz},
            },
        },
        {
            "talas.grainCloud",
            "Grain Cloud",
            "Xtra Sources",
            {
                {"amplitude", talas::core::PortType::Control, false},
                {"pan", talas::core::PortType::Control, false},
                {"frequency", talas::core::PortType::Control, false},
                {"grainDur", talas::core::PortType::Control, false},
                {"density", talas::core::PortType::Control, false},
                {"maxGrains", talas::core::PortType::Control, false},
                {"panJitter", talas::core::PortType::Control, false},
                {"grainDurJitter", talas::core::PortType::Control, false},
                {"freqJitter", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"waveform", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
                {"grainEnv", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"maxGrains", "8", talas::expr::ValueDomain::Scalar},
                {"amplitude", "0.5", talas::expr::ValueDomain::Scalar},
                {"density", "20", talas::expr::ValueDomain::Scalar},
                {"grainDur", "0.1 s", talas::expr::ValueDomain::Seconds},
                {"grainDurJitter", "0.0", talas::expr::ValueDomain::Scalar},
                {"cyclesPerGrain", "20", talas::expr::ValueDomain::Scalar},
                {"frequency", "200 hz", talas::expr::ValueDomain::Hertz},
                {"freqJitter", "0.0", talas::expr::ValueDomain::Scalar},
                {"pan", "0.5", talas::expr::ValueDomain::Scalar},
                {"panJitter", "0.5", talas::expr::ValueDomain::Scalar},
                {"seed", "0.5", talas::expr::ValueDomain::Scalar},
                {"grainDuration", {}, talas::expr::ValueDomain::Scalar, "durPlusCycles",
                    {"durPlusCycles", "legacyDurOrCycles"}},
                {"grainShapes", {}, talas::expr::ValueDomain::Scalar, "atGrainStart",
                    {"atGrainStart", "legacyEveryFrame"}},
            },
        },
        {
            "talas.pulseGenerator",
            "Pulse Generator",
            "Xtra Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"modulator", talas::core::PortType::Audio, false},
                {"envelope", talas::core::PortType::Control, false},
                {"dutyCycle", talas::core::PortType::Control, false},
                {"reset", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"modulation", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"maxMI", "0.0", talas::expr::ValueDomain::Scalar},
                {"interpolation", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "none"}},
                {"envelope", "1.0", talas::expr::ValueDomain::Scalar},
                {"dutyCycle", "0.5", talas::expr::ValueDomain::Scalar},
                {"reset", "0.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.pulseTrain",
            "Pulse Train",
            "Xtra Sources",
            {
                {"period", talas::core::PortType::Control, false},
                {"dutyCycle", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"period", "0.5 s", talas::expr::ValueDomain::Seconds},
                {"variableDutyCycle", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"dutyCycle", "0.5", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.quadOscillator",
            "Quad Oscillator",
            "Xtra Sources",
            {
                {"envelope", talas::core::PortType::Audio, true},
                {"rightEnvelope", talas::core::PortType::Audio, false},
                {"frequency", talas::core::PortType::Control, false},
            },
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {{"frequency", "440 hz", talas::expr::ValueDomain::Hertz}},
        },
        {
            "talas.quadFunctionGenerator",
            "Quad Function Generator",
            "Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"phaseOffset", talas::core::PortType::Control, false},
                {"skew", talas::core::PortType::Control, false},
                {"curve", talas::core::PortType::Control, false},
                {"smooth", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"aOut", talas::core::PortType::Audio, false},
                {"bOut", talas::core::PortType::Audio, false},
                {"cOut", talas::core::PortType::Audio, false},
                {"dOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "1 hz", talas::expr::ValueDomain::Hertz},
                {"phaseOffset", "0.0", talas::expr::ValueDomain::Scalar},
                {"skew", "0.5", talas::expr::ValueDomain::Scalar},
                {"curve", "1.0", talas::expr::ValueDomain::Scalar},
                {"smooth", "0.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.dualMorphOscillator",
            "Dual Morph Oscillator",
            "Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"ratio", talas::core::PortType::Control, false},
                {"phaseMod", talas::core::PortType::Control, false},
                {"morph", talas::core::PortType::Control, false},
                {"fold", talas::core::PortType::Control, false},
                {"amplitude", talas::core::PortType::Control, false},
            },
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"modulatorOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "220 hz", talas::expr::ValueDomain::Hertz},
                {"ratio", "2.0", talas::expr::ValueDomain::Scalar},
                {"phaseMod", "0.0", talas::expr::ValueDomain::Scalar},
                {"morph", "0.0", talas::expr::ValueDomain::Scalar},
                {"fold", "0.0", talas::expr::ValueDomain::Scalar},
                {"amplitude", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.probabilityChaosGenerator",
            "Probability Chaos Generator",
            "Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"probability", talas::core::PortType::Control, false},
                {"chaos", talas::core::PortType::Control, false},
                {"slew", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {
                {"valueOut", talas::core::PortType::Audio, false},
                {"gateOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "4 hz", talas::expr::ValueDomain::Hertz},
                {"probability", "0.5", talas::expr::ValueDomain::Scalar},
                {"chaos", "0.5", talas::expr::ValueDomain::Scalar},
                {"slew", "0 s", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"seed", "1", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.sosOscillators",
            "SOS Oscillators",
            "Xtra Sources",
            {
                {"spectrum", talas::core::PortType::Spectrum, true},
                {"cascadeInput", talas::core::PortType::Audio, true},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"nbrOscillators", "2", talas::expr::ValueDomain::Scalar},
                {"firstOscillator", "1", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
            },
        },
        {
            "talas.sumOfSines",
            "Sum Of Sines",
            "Xtra Sources",
            {
                {"onDuration", talas::core::PortType::Control, false},
                {"frequency0", talas::core::PortType::Control, false},
                {"frequency1", talas::core::PortType::Control, false},
                {"dbMorph", talas::core::PortType::Control, false},
                {"pchMorph", talas::core::PortType::Control, false},
                {"envelope", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"nbrPartials", talas::core::PortType::Control, false},
                {"timeIndex", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"onDuration", "2 s", talas::expr::ValueDomain::Seconds},
                {"frequency0", "0 hz", talas::expr::ValueDomain::Hertz},
                {"frequency1", "0 hz", talas::expr::ValueDomain::Hertz},
                {"analysis0", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "HarmonicAmplitudes", {}, talas::core::ParameterKind::Resource},
                {"analysisFrequency0", "1 hz", talas::expr::ValueDomain::Hertz},
                {"analysisFrames0", "1", talas::expr::ValueDomain::Scalar},
                {"analysis1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"analysisFrequency1", "1 hz", talas::expr::ValueDomain::Hertz},
                {"analysisFrames1", "1", talas::expr::ValueDomain::Scalar},
                {"dbMorph", "0.0", talas::expr::ValueDomain::Scalar},
                {"pchMorph", "0.0", talas::expr::ValueDomain::Scalar},
                {"nbrPartials", "8", talas::expr::ValueDomain::Scalar},
                {"bankSize", "8", talas::expr::ValueDomain::Scalar},
                {"timeIndex", "0.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"loop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"loopStart", "0.0", talas::expr::ValueDomain::Scalar},
                {"loopEnd", "1.0", talas::expr::ValueDomain::Scalar},
                {"ctrlTime", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"envelope", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.twoFormantVoiceElement",
            "Two Formant Voice Element",
            "Xtra Sources",
            {
                {"frequency", talas::core::PortType::Control, false},
                {"formant1", talas::core::PortType::Control, false},
                {"bandwidth1", talas::core::PortType::Control, false},
                {"scale1", talas::core::PortType::Control, false},
                {"formant2", talas::core::PortType::Control, false},
                {"bandwidth2", talas::core::PortType::Control, false},
                {"scale2", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "110 hz", talas::expr::ValueDomain::Hertz},
                {"formant1", "238 hz", talas::expr::ValueDomain::Hertz},
                {"bandwidth1", "70 hz", talas::expr::ValueDomain::Hertz},
                {"scale1", "0.3", talas::expr::ValueDomain::Scalar},
                {"formant2", "1741 hz", talas::expr::ValueDomain::Hertz},
                {"bandwidth2", "100 hz", talas::expr::ValueDomain::Hertz},
                {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
                {"seed", "1", talas::expr::ValueDomain::Scalar},
                {"excitation", {}, talas::expr::ValueDomain::Scalar, "glottalPulse", {"glottalPulse", "legacySine"}},
            },
        },
        {
            "talas.channelCrosser",
            "Channel Crosser",
            "Mixing & Panning",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"leftInLeft", talas::core::PortType::Control, false},
                {"rightInLeft", talas::core::PortType::Control, false},
                {"leftInRight", talas::core::PortType::Control, false},
                {"rightInRight", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"leftInLeft", "1.0", talas::expr::ValueDomain::Scalar},
                {"rightInLeft", "0.0", talas::expr::ValueDomain::Scalar},
                {"leftInRight", "0.0", talas::expr::ValueDomain::Scalar},
                {"rightInRight", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.channelJoin",
            "Channel Join",
            "Mixing & Panning",
            {
                {"left", talas::core::PortType::Audio, true},
                {"right", talas::core::PortType::Audio, true},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.complexProduct",
            "Complex Product",
            "Math",
            {
                {"input0Left", talas::core::PortType::Audio, true},
                {"input0Right", talas::core::PortType::Audio, true},
                {"input1Left", talas::core::PortType::Audio, true},
                {"input1Right", talas::core::PortType::Audio, true},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.clippingDivide",
            "Clipping Divide",
            "Math",
            {
                {"numerator", talas::core::PortType::Audio, true},
                {"denominator", talas::core::PortType::Audio, true},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {{"scale", "1.0", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.minimum",
            "Minimum",
            "Math",
            {
                {"input0Left", talas::core::PortType::Audio, true},
                {"input0Right", talas::core::PortType::Audio, true},
                {"input1Left", talas::core::PortType::Audio, true},
                {"input1Right", talas::core::PortType::Audio, true},
                {"input2Left", talas::core::PortType::Audio, true},
                {"input2Right", talas::core::PortType::Audio, true},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.maximum",
            "Maximum",
            "Math",
            {
                {"input0Left", talas::core::PortType::Audio, true},
                {"input0Right", talas::core::PortType::Audio, true},
                {"input1Left", talas::core::PortType::Audio, true},
                {"input1Right", talas::core::PortType::Audio, true},
                {"input2Left", talas::core::PortType::Audio, true},
                {"input2Right", talas::core::PortType::Audio, true},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {},
        },
        {
            "talas.channeller",
            "Channeller",
            "Mixing & Panning",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
                {"leftChannel", talas::core::PortType::Control, false},
                {"rightChannel", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"leftChannel", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"rightChannel", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
            },
        },
        {
            "talas.overlappingMixer",
            "Overlapping Mixer",
            "Mixing & Panning",
            {
                {"in1", talas::core::PortType::Audio, false},
                {"in2", talas::core::PortType::Audio, false},
                {"in3", talas::core::PortType::Audio, false},
                {"in4", talas::core::PortType::Audio, false},
                {"inputs", talas::core::PortType::Audio, false},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"legacyInputs", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"overlapTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"sequence", {}, talas::expr::ValueDomain::Scalar, "oneAfterAnother", {"oneAfterAnother", "legacyAllAtOnce"}},
            },
        },
        {
            "talas.stereoMix4",
            "Stereo Mix 4",
            "Mixing & Panning",
            {
                {"in1", talas::core::PortType::Audio, true},
                {"in2", talas::core::PortType::Audio, true},
                {"in3", talas::core::PortType::Audio, true},
                {"in4", talas::core::PortType::Audio, true},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
                {"pan1", talas::core::PortType::Control, false},
                {"scale1", talas::core::PortType::Control, false},
                {"pan2", talas::core::PortType::Control, false},
                {"scale2", talas::core::PortType::Control, false},
                {"pan3", talas::core::PortType::Control, false},
                {"scale3", talas::core::PortType::Control, false},
                {"pan4", talas::core::PortType::Control, false},
                {"scale4", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"pan1", "0.0", talas::expr::ValueDomain::Scalar},
                {"scale1", "1.0", talas::expr::ValueDomain::Scalar},
                {"pan2", "0.33", talas::expr::ValueDomain::Scalar},
                {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
                {"pan3", "0.67", talas::expr::ValueDomain::Scalar},
                {"scale3", "1.0", talas::expr::ValueDomain::Scalar},
                {"pan4", "1.0", talas::expr::ValueDomain::Scalar},
                {"scale4", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.frequencyScale",
            "Frequency Scale",
            "Frequency & Time Scaling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"freqTracker", talas::core::PortType::Control, true},
                {"frequencyScale", talas::core::PortType::Control, false},
                {"delay", talas::core::PortType::Control, false},
                {"maxScale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequencyScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"maxScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"window", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"delay", "0", talas::expr::ValueDomain::Scalar},
                {"trackerUnits", {}, talas::expr::ValueDomain::Scalar, "fractionOfNyquist",
                    {"fractionOfNyquist", "legacyHertz"}},
                {"formants", {}, talas::expr::ValueDomain::Scalar, "stay", {"stay", "legacyMove"}},
                {"scaling", {}, talas::expr::ValueDomain::Scalar, "grains", {"grains", "legacyVocoder"}},
                {"delayRead", {}, talas::expr::ValueDomain::Scalar, "atStart", {"atStart", "legacyFollows"}},
            },
        },
        {
            "talas.monotonizer",
            "Monotonizer",
            "Frequency & Time Scaling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"frequency", talas::core::PortType::Control, false},
                {"minInputPitch", talas::core::PortType::Control, false},
                {"maxInputPitch", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"minInputPitch", "440 hz", talas::expr::ValueDomain::Hertz},
                {"maxInputPitch", "440 hz", talas::expr::ValueDomain::Hertz},
            },
        },
        {
            "talas.simplePitchShifter",
            "Simple Pitch Shifter",
            "Frequency & Time Scaling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"interval", talas::core::PortType::Control, false},
                {"minInputPitch", talas::core::PortType::Control, false},
                {"maxInputPitch", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"interval", "0", talas::expr::ValueDomain::Scalar},
                {"minInputPitch", "20 hz", talas::expr::ValueDomain::Hertz},
                {"maxInputPitch", "2000 hz", talas::expr::ValueDomain::Hertz},
            },
        },
        {
            "talas.singleSideBandRm",
            "Single Side Band RM",
            "Frequency & Time Scaling",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"frequency", talas::core::PortType::Control, false},
                {"freqScale", talas::core::PortType::Control, false},
                {"rightIn", talas::core::PortType::Audio, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"freqScale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.reverbSection",
            "Reverb Section",
            "Reverb, Delay, Feedback",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"decayTime", talas::core::PortType::Control, false},
                {"delay", talas::core::PortType::Control, false},
                {"delayScale", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"type", {}, talas::expr::ValueDomain::Scalar, "comb", {"comb", "allpass"}},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"decayTime", "1 s", talas::expr::ValueDomain::Seconds},
                {"delay", "250 ms", talas::expr::ValueDomain::Seconds},
                {"delayScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Private", {}, talas::core::ParameterKind::Resource},
                {"prezero", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"interpolation", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "none"}},
                {"smoothDelayChanges", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
            },
        },
        {
            "talas.soundCollectionVariable",
            "Sound Collection Variable",
            "Variables",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"variableId", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"members", {}, talas::expr::ValueDomain::Scalar, "all", {"all", "legacySelected"}},
            },
        },
        {
            "talas.variable",
            "Variable",
            "Variables & Annotation",
            {},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {{"variableId", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text}},
        },
        {
            "talas.timeFrequencyScale",
            "Time Frequency Scale",
            "Frequency & Time Scaling",
            {
                {"frequencyScale", talas::core::PortType::Control, false},
                {"rate", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"minInputFreq", talas::core::PortType::Control, false},
                {"maxInputFreq", talas::core::PortType::Control, false},
                {"maxFreqScale", talas::core::PortType::Control, false},
                {"detectors", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"frequencyScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"minInputFreq", "100 hz", talas::expr::ValueDomain::Hertz},
                {"maxInputFreq", "2000 hz", talas::expr::ValueDomain::Hertz},
                {"maxFreqScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"detectors", "10", talas::expr::ValueDomain::Scalar},
                {"fromDisk", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"sample", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"rateAboveOne", {}, talas::expr::ValueDomain::Scalar, "refused", {"refused", "legacyCompresses"}},
            },
        },
        {
            "talas.contextFreeGrammar",
            "Context Free Grammar",
            "Scripts",
            {{"inputs", talas::core::PortType::Audio, false}},
            {{"audioOut", talas::core::PortType::Audio, false}, {"rightOut", talas::core::PortType::Audio, false}},
            {
                {"rewriteRules", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"startExpression", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"seed", "0", talas::expr::ValueDomain::Scalar},
                {"maxRewrites", "0", talas::expr::ValueDomain::Scalar},
                {"maxSize", "64", talas::expr::ValueDomain::Scalar},
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.limeInterpreter",
            "Lime Interpreter",
            "Scripts",
            {
                {"inputs", talas::core::PortType::Audio, false},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"fileName", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Resource},
                {"script", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.analogSequencer",
            "Analog Sequencer",
            "Sequencers",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"step", talas::core::PortType::Control, false},
                {"rate", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"step", "0", talas::expr::ValueDomain::Scalar},
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
                {"loop", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"startIndex", "0", talas::expr::ValueDomain::Scalar},
                {"endIndex", "3", talas::expr::ValueDomain::Scalar},
                {"polyphony", "1", talas::expr::ValueDomain::Scalar},
                {"durations", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0.25 s} {0.25 s} {0.25 s} {0.25 s}", talas::core::ParameterKind::Text},
                {"dutyCycles", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0.5}", talas::core::ParameterKind::Text},
                {"pitches", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"velocities", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"extraValues", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"keyDowns", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"keyPitches", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"keyVelocities", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"keyTimbres", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"indexOrigin", {}, talas::expr::ValueDomain::Scalar, "zeroBased", {"zeroBased", "legacyOneBased"}},
                {"generatedIndex", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"stepZeroMode", {}, talas::expr::ValueDomain::Scalar, "waitForTrigger", {"waitForTrigger", "legacyAutomatic"}},
                {"stepMode", {}, talas::expr::ValueDomain::Scalar, "trigger", {"trigger", "legacyStageNumber"}},
                {"dutyCycleOne", {}, talas::expr::ValueDomain::Scalar, "holds", {"holds", "legacyRetriggers"}},
                {"constantFieldsRead", {}, talas::expr::ValueDomain::Scalar, "eachVoice", {"eachVoice", "legacyWhenCompiled"}},
            },
        },
        {
            "talas.parameterTransformer",
            "Parameter Transformer",
            "Scripts",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"transformation", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
            },
        },
        {
            "talas.script",
            "Script",
            "Scripts",
            {
                {"audioIn", talas::core::PortType::Audio, false},
                {"inputs", talas::core::PortType::Audio, false},
                {"left", talas::core::PortType::Control, false},
                {"right", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"script", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"left", "1.0", talas::expr::ValueDomain::Scalar},
                {"right", "1.0", talas::expr::ValueDomain::Scalar},
                {"keywordSpacing", {}, talas::expr::ValueDomain::Scalar, "spaceAfterColon", {"spaceAfterColon", "legacyTouching"}},
            },
        },
        {
            "talas.functionGenerator",
            "Function Generator",
            "Envelopes & Control Signals",
            {
                {"trigger", talas::core::PortType::Control, false},
                {"onDuration", talas::core::PortType::Control, false},
                {"level", talas::core::PortType::Audio, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"onDuration", "1 s", talas::expr::ValueDomain::Seconds},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.filter",
            "Filter",
            "Filters",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"frequency", talas::core::PortType::Control, false},
                {"q", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"feedback", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"type", {}, talas::expr::ValueDomain::Scalar, "lowPass", {"lowPass", "highPass", "allPass"}},
                {"frequency", "1000 hz", talas::expr::ValueDomain::Hertz},
                {"q", "1.0", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"feedback", "0.0", talas::expr::ValueDomain::Scalar},
                {"order", "4", talas::expr::ValueDomain::Scalar},
                {"poles", {}, talas::expr::ValueDomain::Scalar, "order", {"order", "legacyTwoPerOrder"}},
                {"negativeFeedback", {}, talas::expr::ValueDomain::Scalar, "invertsPhase", {"invertsPhase", "legacyTurnsSignOver"}},
                {"response", {}, talas::expr::ValueDomain::Scalar, "onePoleSections", {"onePoleSections", "butterworth"}},
            },
        },
        {
            "talas.adsr",
            "ADSR",
            "Envelopes & Control Signals",
            {{"gate", talas::core::PortType::Control, false}},
            {{"value", talas::core::PortType::Control, false}},
            {
                {"attackTime", "10 ms", talas::expr::ValueDomain::Seconds},
                {"decayTime", "100 ms", talas::expr::ValueDomain::Seconds},
                {"sustainLevel", "0.7", talas::expr::ValueDomain::Scalar},
                {"releaseTime", "250 ms", talas::expr::ValueDomain::Seconds},
                {"type", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "exponential"}},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"legato", "0.0", talas::expr::ValueDomain::Scalar},
                {"negativeGate", {}, talas::expr::ValueDomain::Scalar, "decaysRapidly", {"decaysRapidly", "legacyReleases"}},
                {"constantGate", {}, talas::expr::ValueDomain::Scalar, "endsWithTheSound", {"endsWithTheSound", "legacyHolds"}},
            },
        },
        {
            "talas.ar",
            "AR",
            "Envelopes & Control Signals",
            {{"gate", talas::core::PortType::Control, false}},
            {{"value", talas::core::PortType::Control, false}},
            {
                {"attackTime", "10 ms", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "250 ms", talas::expr::ValueDomain::Seconds},
                {"type", {}, talas::expr::ValueDomain::Scalar, "linear", {"linear", "exponential"}},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"negativeGate", {}, talas::expr::ValueDomain::Scalar, "decaysRapidly", {"decaysRapidly", "legacyReleases"}},
            },
        },
    };

    for (const auto& expected : expectedDescriptors)
    {
        const auto& descriptor = requireNodeType(registry, expected.typeId);
        require(descriptor.displayName == expected.displayName, expected.typeId + " should declare expected display name");
        require(descriptor.category == expected.category, expected.typeId + " should declare expected category");
        requirePorts(descriptor.inputs, expected.inputs, expected.typeId, "input");
        requirePorts(descriptor.outputs, expected.outputs, expected.typeId, "output");
        requireParameters(descriptor.parameters, expected.parameters, expected.typeId);
    }

    const auto& audioOutput = requireNodeType(
        registry, "talas.audioOutput");
    require(
        audioOutput.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && audioOutput.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && audioOutput.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && audioOutput.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && audioOutput.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !audioOutput.capabilities.supportsReset
            && audioOutput.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && audioOutput.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && audioOutput.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && audioOutput.capabilities.outputBusLayouts
                == std::vector{
                    talas::core::AudioBusLayoutKind::Mono,
                    talas::core::AudioBusLayoutKind::Stereo}
            && std::ranges::find(
                audioOutput.capabilities.limitations,
                "allocation-free-whole-callback-candidate-publication")
                != audioOutput.capabilities.limitations.end(),
        "C138 Audio Output should expose supported exact mono/stereo terminal authority");
}

void testExactMathDescriptorsDeclareControlContracts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<ExpectedNodeDescriptor> expectedDescriptors = {
        {
            "talas.difference",
            "Difference",
            "Math",
            {{"left", talas::core::PortType::Control, true}, {"right", talas::core::PortType::Control, true}},
            {{"value", talas::core::PortType::Control, false}},
            {},
        },
        {
            "talas.arcTan",
            "ArcTan",
            "Math",
            {{"value", talas::core::PortType::Control, true}, {"rightValue", talas::core::PortType::Control, false}},
            {{"value", talas::core::PortType::Control, false}},
            {
                {"angle", {}, talas::expr::ValueDomain::Scalar, "rightOverLeft", {"rightOverLeft", "legacyRadians"}},
            },
        },
        {
            "talas.linearToLog",
            "Linear To Log",
            "Math",
            {{"value", talas::core::PortType::Control, false}, {"audioIn", talas::core::PortType::Audio, false}},
            {{"value", talas::core::PortType::Control, false}, {"audioOut", talas::core::PortType::Audio, false}},
            {},
        },
        {
            "talas.logToLinear",
            "Log To Linear",
            "Math",
            {{"value", talas::core::PortType::Control, false}, {"audioIn", talas::core::PortType::Audio, false}},
            {{"value", talas::core::PortType::Control, false}, {"audioOut", talas::core::PortType::Audio, false}},
            {},
        },
        {
            "talas.equality",
            "Equality",
            "Math",
            {{"left", talas::core::PortType::Control, true}, {"right", talas::core::PortType::Control, true}},
            {{"value", talas::core::PortType::Control, false}},
            {{"tolerance", "0", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.runningMax",
            "Running Max",
            "Math",
            {{"value", talas::core::PortType::Control, true}, {"reset", talas::core::PortType::Control, false}},
            {{"value", talas::core::PortType::Control, false}},
            {{"reset", "0.0", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.runningMin",
            "Running Min",
            "Math",
            {
                {"value", talas::core::PortType::Control, true},
                {"reset", talas::core::PortType::Control, false},
                {"maximumAmplitude", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"reset", "0.0", talas::expr::ValueDomain::Scalar},
                {"maximumAmplitude", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.phaseShiftBy90",
            "Phase Shift By 90",
            "Math",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"frequency", talas::core::PortType::Control, false},
                {"rightIn", talas::core::PortType::Audio, false},
            },
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"frequency", "440 hz", talas::expr::ValueDomain::Hertz},
                {"overflow", {}, talas::expr::ValueDomain::Scalar, "clip", {"clip", "unclipped"}},
            },
        },
        {
            "talas.setRange",
            "Set Range",
            "Math",
            {
                {"value", talas::core::PortType::Control, true},
                {"oldMin", talas::core::PortType::Control, false},
                {"oldMax", talas::core::PortType::Control, false},
                {"newMin", talas::core::PortType::Control, false},
                {"newMax", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"oldMin", "0.0", talas::expr::ValueDomain::Scalar},
                {"oldMax", "1.0", talas::expr::ValueDomain::Scalar},
                {"newMin", "0.0", talas::expr::ValueDomain::Scalar},
                {"newMax", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.sqrtMagnitude",
            "Sqrt Magnitude",
            "Math",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {},
        },
        {
            "talas.scaleAndOffset",
            "Scale and Offset",
            "Math",
            {
                {"value", talas::core::PortType::Control, true},
                {"rightValue", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
                {"offset", talas::core::PortType::Control, false},
                {"rightScale", talas::core::PortType::Control, false},
                {"rightOffset", talas::core::PortType::Control, false},
            },
            {
                {"value", talas::core::PortType::Control, false},
                {"rightValue", talas::core::PortType::Control, false},
            },
            {
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"offset", "0.0", talas::expr::ValueDomain::Scalar},
                {"rightScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"rightOffset", "0.0", talas::expr::ValueDomain::Scalar},
            },
        },
    };

    for (const auto& expected : expectedDescriptors)
    {
        const auto& descriptor = requireNodeType(registry, expected.typeId);
        require(descriptor.displayName == expected.displayName, expected.typeId + " should declare expected display name");
        require(descriptor.category == expected.category, expected.typeId + " should declare expected category");
        requirePorts(descriptor.inputs, expected.inputs, expected.typeId, "input");
        requirePorts(descriptor.outputs, expected.outputs, expected.typeId, "output");
        requireParameters(descriptor.parameters, expected.parameters, expected.typeId);
    }
}

void testTimingAndTrackingDescriptorsDeclareCatalogContracts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<ExpectedNodeDescriptor> expectedDescriptors = {
        {
            "talas.amplitudeFollower",
            "Amplitude Follower",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"timeConstant", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"timeConstant", "100 ms", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.biDiFollower",
            "BiDi Follower",
            "Tracking Live Input",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"attackTime", "100 ms", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "100 ms", talas::expr::ValueDomain::Seconds},
            },
        },
        {
            "talas.lossyIntegrator",
            "Lossy Integrator",
            "Tracking Live Input",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"releaseTime", "1 s", talas::expr::ValueDomain::Seconds},
            },
        },
        {
            "talas.hysteresis",
            "Hysteresis",
            "Tracking Live Input",
            {
                {"value", talas::core::PortType::Control, false},
                {"audioIn", talas::core::PortType::Audio, false},
            },
            {
                {"value", talas::core::PortType::Control, false},
                {"audioOut", talas::core::PortType::Audio, false},
            },
            {{"width", "0.25", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.binauralPan",
            "Binaural Pan",
            "Spatializing",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"pan", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {{"pan", "0.5", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.splitSurroundSampleWithTimeIndex",
            "Split Surround Sample With Time Index",
            "Sampling",
            {{"timeIndex", talas::core::PortType::Audio, true}},
            {
                {"out1", talas::core::PortType::Audio, false},
                {"out2", talas::core::PortType::Audio, false},
                {"out3", talas::core::PortType::Audio, false},
                {"out4", talas::core::PortType::Audio, false},
                {"out5", talas::core::PortType::Audio, false},
                {"out6", talas::core::PortType::Audio, false},
                {"out7", talas::core::PortType::Audio, false},
                {"out8", talas::core::PortType::Audio, false},
            },
            {
                {"fileNames", {}, talas::expr::ValueDomain::Scalar,
                 {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"interpolation", {}, talas::expr::ValueDomain::Scalar,
                 "linear", {"none", "linear"}},
            },
        },
        {
            "talas.analysisFilter",
            "Analysis Filter",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"fundamental", talas::core::PortType::Control, false},
                {"harmonic", talas::core::PortType::Control, false},
            },
            {
                {"leftOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"fundamental", "440 hz", talas::expr::ValueDomain::Hertz},
                {"harmonic", "1", talas::expr::ValueDomain::Scalar},
                {"design", {}, talas::expr::ValueDomain::Scalar, "harmonicWindow",
                    {"harmonicWindow", "legacyBandpass"}},
            },
        },
        {
            "talas.chopper",
            "Chopper",
            "Envelopes & Control Signals",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"grainDuration", talas::core::PortType::Control, false},
                {"interGrainDelay", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"grainDuration", "0.1 s", talas::expr::ValueDomain::Seconds},
                {"interGrainDelay", "0.1 s", talas::expr::ValueDomain::Seconds},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Hann", {}, talas::core::ParameterKind::Resource},
                {"timing", {}, talas::expr::ValueDomain::Scalar, "grainDurationAndDelay",
                    {"grainDurationAndDelay", "rateAndDutyCycle"}},
                {"rate", "5 hz", talas::expr::ValueDomain::Hertz},
                {"dutyCycle", "0.5", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.frequencyTracker",
            "Frequency Tracker",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"minFrequency", talas::core::PortType::Control, false},
                {"maxFrequency", talas::core::PortType::Control, false},
                {"confidence", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}, {"rightOut", talas::core::PortType::Control, false}},
            {
                {"minFrequency", "20 hz", talas::expr::ValueDomain::Hertz},
                {"maxFrequency", "2000 hz", talas::expr::ValueDomain::Hertz},
                {"confidence", "0.5", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"detectors", "10", talas::expr::ValueDomain::Scalar},
                {"emphasis", "0.0", talas::expr::ValueDomain::Scalar},
                {"outputUnits", {}, talas::expr::ValueDomain::Scalar, "fractionOfNyquist",
                    {"fractionOfNyquist", "legacyHertz"}},
                {"scaleAttenuates", {}, talas::expr::ValueDomain::Scalar, "input", {"input", "legacyEstimate"}},
            },
        },
        {
            "talas.graphicalEnvelope",
            "Graphical Envelope",
            "Envelopes & Control Signals",
            {
                {"level", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"rate", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"envelope", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0 s} {0} {0.1 s} {1} {1 s} {0}", talas::core::ParameterKind::Text},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
                {"negativeGate", {}, talas::expr::ValueDomain::Scalar, "decaysRapidly", {"decaysRapidly", "legacyReleases"}},
            },
        },
        {
            "talas.liveSpectralAnalysis",
            "Live Spectral Analysis",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"ampScale", talas::core::PortType::Control, false},
                {"freqScale", talas::core::PortType::Control, false},
            },
            {{"spectrumOut", talas::core::PortType::Spectrum, false}},
            {
                {"lowestAnalyzedFreq", "100 hz", talas::expr::ValueDomain::Hertz},
                {"ampScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"freqScale", "1.0", talas::expr::ValueDomain::Scalar},
                {"response", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"harmonic", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"unpitchedOnly", "0", talas::expr::ValueDomain::Scalar},
                {"unpitchedThreshold", "0", talas::expr::ValueDomain::Scalar},
                {"trackedHarmonic", "1", talas::expr::ValueDomain::Scalar},
                {"lowFreq", "50 hz", talas::expr::ValueDomain::Hertz},
                {"highFreq", "2000 hz", talas::expr::ValueDomain::Hertz},
                {"initFreq", "440 hz", talas::expr::ValueDomain::Hertz},
                {"fundamentalOnly", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"tracks", {}, talas::expr::ValueDomain::Scalar, "bands",
                    {"bands", "legacyPeaks"}},
                {"bandWidth", {}, talas::expr::ValueDomain::Scalar, "nearestPower",
                    {"nearestPower", "legacyNextPower"}},
            },
        },
        {
            "talas.multisegmentEnvelope",
            "Multisegment Envelope",
            "Envelopes & Control Signals",
            {
                {"level", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"durations", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0.1 s} {0.4 s} {0.5 s}", talas::core::ParameterKind::Text},
                {"breakPoints", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0} {1} {0.5} {0}", talas::core::ParameterKind::Text},
                {"startLoop", "0", talas::expr::ValueDomain::Scalar},
                {"endLoop", "0", talas::expr::ValueDomain::Scalar},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"loopRelease", {}, talas::expr::ValueDomain::Scalar, "onGateRelease", {"onGateRelease", "legacyNever"}},
                {"negativeGate", {}, talas::expr::ValueDomain::Scalar, "decaysRapidly", {"decaysRapidly", "legacyReleases"}},
            },
        },
        {
            "talas.multislopeFunctionGenerator",
            "Multislope Function Generator",
            "Envelopes & Control Signals",
            {
                {"level", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
                {"rate", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"timePoints", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0 s} {0.5 s} {1 s}", talas::core::ParameterKind::Text},
                {"slopes", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{-1} {1}", talas::core::ParameterKind::Text},
                {"level", "1.0", talas::expr::ValueDomain::Scalar},
                {"gate", "1", talas::expr::ValueDomain::Scalar},
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.peakDetector",
            "Peak Detector",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"attackTime", talas::core::PortType::Control, false},
                {"releaseTime", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"attackTime", "10 ms", talas::expr::ValueDomain::Seconds},
                {"releaseTime", "100 ms", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.rmsSquared",
            "RMS Squared",
            "Tracking Live Input",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"timeConstant", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"timeConstant", "100 ms", talas::expr::ValueDomain::Seconds},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.soundToGlobalController",
            "Sound To Global Controller",
            "Tracking Live Input",
            {
                {"value", talas::core::PortType::Control, false},
            },
            {},
            {
                {"generatedEvent", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"value", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
            },
        },
        {
            "talas.threshold",
            "Threshold",
            "Tracking Live Input",
            {
                {"value", talas::core::PortType::Control, true},
                {"threshold", talas::core::PortType::Control, false},
                {"hysteresis", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"threshold", "0.5", talas::expr::ValueDomain::Scalar},
                {"hysteresis", "0.0", talas::expr::ValueDomain::Scalar},
                {"hysteresisBand", {}, talas::expr::ValueDomain::Scalar, "full", {"full", "legacyHalf"}},
            },
        },
        {
            "talas.setDuration",
            "Set Duration",
            "Time & Duration",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"startTime", talas::core::PortType::Control, false},
                {"duration", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"startTime", "0 s", talas::expr::ValueDomain::Seconds},
                {"duration", "1 s", talas::expr::ValueDomain::Seconds},
                {"startTimeMeans", {}, talas::expr::ValueDomain::Scalar, "startsInputLate",
                    {"startsInputLate", "legacyDelay", "legacyFixedStartsLate", "legacyFlatVoicesOnly",
                        "legacyDurationUnpaced"}},
            },
        },
        {
            "talas.triggeredTableRead",
            "Triggered Table Read",
            "Sampling",
            {
                {"trigger", talas::core::PortType::Control, false},
                {"gate", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "Sine", {}, talas::core::ParameterKind::Resource},
                {"gate", "1.0", talas::expr::ValueDomain::Scalar},
                {"ignoreLoops", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"headerLoop", {}, talas::expr::ValueDomain::Scalar, "loop", {"loop", "legacyWholeTable"}},
            },
        },
        {
            "talas.triggeredSampleAndHold",
            "Triggered Sample And Hold",
            "Sampling",
            {{"value", talas::core::PortType::Control, true}, {"trigger", talas::core::PortType::Control, false}},
            {{"value", talas::core::PortType::Control, false}},
            {{"trigger", "1.0", talas::expr::ValueDomain::Scalar}},
        },
        {
            "talas.inputOutputCharacteristic",
            "Input Output Characteristic",
            "Distortion & Waveshaping",
            {{"audioIn", talas::core::PortType::Audio, true}},
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"inValues", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "-1 0 1", talas::core::ParameterKind::Text},
                {"outValues", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "-1 0 1", talas::core::ParameterKind::Text},
                {"smoothing", "0.5", talas::expr::ValueDomain::Scalar},
                {"startSlope", "1.0", talas::expr::ValueDomain::Scalar},
                {"endSlope", "1.0", talas::expr::ValueDomain::Scalar},
            },
        },
        {
            "talas.iteratedWaveshaper",
            "Iterated Waveshaper",
            "Distortion & Waveshaping",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"iterations", talas::core::PortType::Control, false},
                {"scale", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"iterations", "1", talas::expr::ValueDomain::Scalar},
                {"scale", "1.0", talas::expr::ValueDomain::Scalar},
                {"wavetable", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "FullRamp", {}, talas::core::ParameterKind::Resource},
            },
        },
        {
            "talas.waveshaper",
            "Waveshaper",
            "Distortion & Waveshaping",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
            },
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"interpolation", {}, talas::expr::ValueDomain::Scalar, "linear", {"none", "linear"}},
                {"shapeFrom", {}, talas::expr::ValueDomain::Scalar, "wavetable", {"wavetable", "polynomial"}},
                {"shapingFunction", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, "FullRamp", {}, talas::core::ParameterKind::Resource},
                {"coefficients", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text},
                {"fromMemoryWriter", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"integerIndex", {}, talas::expr::ValueDomain::Scalar, "truncate", {"truncate", "legacyRound"}},
            },
        },
        {
            "talas.timeControl",
            "Time Control",
            "Time & Duration",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rate", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
                {"rateChanges", {}, talas::expr::ValueDomain::Scalar, "eventTimes", {"eventTimes", "legacyPlaybackSpeed"}},
            },
        },
        {
            "talas.timeStopper",
            "Time Stopper",
            "Time & Duration",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"resume", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"resume", "1.0", talas::expr::ValueDomain::Scalar},
                {"resumeOnZero", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"whileStopped", {}, talas::expr::ValueDomain::Scalar, "inputPlays", {"inputPlays", "legacyHold"}},
            },
        },
        {
            "talas.timeOffset",
            "Time Offset",
            "Time & Duration",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"offset", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"offset", "0 s", talas::expr::ValueDomain::Seconds},
                {"reverse", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"retrograde", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"reverseMeans", {}, talas::expr::ValueDomain::Scalar, "silenceFollows",
                    {"silenceFollows", "legacyReverseWindows"}},
                {"offsetMeans", {}, talas::expr::ValueDomain::Scalar, "startsInputLate",
                    {"startsInputLate", "legacyDelay", "legacyFixedStartsLate", "legacyFlatVoicesOnly"}},
            },
        },
        {
            "talas.waitUntil",
            "Wait Until",
            "Time & Duration",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"resume", talas::core::PortType::Control, false},
            },
            {{"audioOut", talas::core::PortType::Audio, false}},
            {
                {"resume", "1.0", talas::expr::ValueDomain::Scalar},
                {"resumeOnZero", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.warpedTimeIndex",
            "Warped Time Index",
            "Time & Duration",
            {
                {"trigger", talas::core::PortType::Control, false},
                {"rate", talas::core::PortType::Control, false},
                {"morph", talas::core::PortType::Control, false},
            },
            {{"value", talas::core::PortType::Control, false}},
            {
                {"idealTimePoints0", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0 s} {1 s}", talas::core::ParameterKind::Text},
                {"idealTimePoints1", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0 s} {1 s}", talas::core::ParameterKind::Text},
                {"currentTimePoints", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, "{0 s} {1 s}", talas::core::ParameterKind::Text},
                {"trigger", "1.0", talas::expr::ValueDomain::Scalar},
                {"rate", "1.0", talas::expr::ValueDomain::Scalar},
                {"morph", "0.0", talas::expr::ValueDomain::Scalar},
            },
        },
    };

    for (const auto& expected : expectedDescriptors)
    {
        const auto& descriptor = requireNodeType(registry, expected.typeId);
        require(descriptor.displayName == expected.displayName, expected.typeId + " should declare expected display name");
        require(descriptor.category == expected.category, expected.typeId + " should declare expected category");
        requirePorts(descriptor.inputs, expected.inputs, expected.typeId, "input");
        requirePorts(descriptor.outputs, expected.outputs, expected.typeId, "output");
        requireParameters(descriptor.parameters, expected.parameters, expected.typeId);
    }
}

void testIoDescriptorsDeclareCatalogContracts()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const std::vector<ExpectedNodeDescriptor> expectedDescriptors = {
        {
            "talas.audioInput",
            "Audio Input",
            "Sampling",
            {},
            {
                {"audioOut", talas::core::PortType::Audio, false},
                {"rightOut", talas::core::PortType::Audio, false},
            },
            {
                {"routing", {}, talas::expr::ValueDomain::Scalar,
                 "monoSum", {"monoSum", "stereoPair"}},
                {"channel1", {}, talas::expr::ValueDomain::Scalar, {}, {}, true},
                {"channel2", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel3", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel4", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel5", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel6", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel7", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
                {"channel8", {}, talas::expr::ValueDomain::Scalar, {}, {}, false},
            },
        },
        {
            "talas.audioOutput",
            "Audio Output",
            "IO",
            {
                {"audioIn", talas::core::PortType::Audio, true},
                {"rightIn", talas::core::PortType::Audio, false},
            },
            {},
            {},
        },
    };

    for (const auto& expected : expectedDescriptors)
    {
        const auto& descriptor = requireNodeType(registry, expected.typeId);
        require(descriptor.displayName == expected.displayName, expected.typeId + " should declare expected display name");
        require(descriptor.category == expected.category, expected.typeId + " should declare expected category");
        requirePorts(descriptor.inputs, expected.inputs, expected.typeId, "input");
        requirePorts(descriptor.outputs, expected.outputs, expected.typeId, "output");
        requireParameters(descriptor.parameters, expected.parameters, expected.typeId);
    }
}

void testPanDescriptorDeclaresExactContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.pan");

    requirePorts(
        descriptor.inputs,
        {
            {"audioIn", talas::core::PortType::Audio, true},
            {"pan", talas::core::PortType::Control, false},
            {"scale", talas::core::PortType::Control, false},
        },
        descriptor.typeId,
        "input");
    requirePorts(
        descriptor.outputs,
        {
            {"leftOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
            {"out1", talas::core::PortType::Audio, false},
            {"out2", talas::core::PortType::Audio, false},
            {"out3", talas::core::PortType::Audio, false},
            {"out4", talas::core::PortType::Audio, false},
            {"out5", talas::core::PortType::Audio, false},
            {"out6", talas::core::PortType::Audio, false},
            {"out7", talas::core::PortType::Audio, false},
            {"out8", talas::core::PortType::Audio, false},
        },
        descriptor.typeId,
        "output");
    requireParameters(
        descriptor.parameters,
        {
            {"pan", "0.0", talas::expr::ValueDomain::Scalar},
            {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            {"type", {}, talas::expr::ValueDomain::Scalar, "power", {"power", "linear"}},
            {"layout", {}, talas::expr::ValueDomain::Scalar, "stereo", {"stereo", "discrete4", "discrete8"}},
        },
        descriptor.typeId);
    require(
        descriptor.capabilities.outputBusLayouts
            == std::vector{
                talas::core::AudioBusLayoutKind::Stereo,
                talas::core::AudioBusLayoutKind::Discrete4,
                talas::core::AudioBusLayoutKind::Discrete8,
            },
        "Pan should advertise its exact stereo and discrete output layouts");
    require(
        descriptor.inputs[1].signal.rate
                == talas::core::PortSignalRate::Sample
            && descriptor.inputs[2].signal.rate
                == talas::core::PortSignalRate::Sample
            && descriptor.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !descriptor.capabilities.supportsReset
            && descriptor.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && std::ranges::find(
                descriptor.capabilities.limitations,
                "allocation-free-whole-layout-whole-block-failure-atomic-retry")
                != descriptor.capabilities.limitations.end(),
        "C134 Pan should expose supported checked sample-rate multi-layout authority");
}

void testCrossfadeDescriptorDeclaresExactContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.crossfade");

    requirePorts(
        descriptor.inputs,
        {
            {"snd1", talas::core::PortType::Audio, true},
            {"snd2", talas::core::PortType::Audio, true},
            {"pan", talas::core::PortType::Control, false},
            {"scale", talas::core::PortType::Control, false},
            {"fade", talas::core::PortType::Control, false},
        },
        descriptor.typeId,
        "input");
    requirePorts(
        descriptor.outputs,
        {
            {"leftOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
        },
        descriptor.typeId,
        "output");
    requireParameters(
        descriptor.parameters,
        {
            {"pan", "0.0", talas::expr::ValueDomain::Scalar},
            {"scale", "1.0", talas::expr::ValueDomain::Scalar},
            {"fade", "0.0", talas::expr::ValueDomain::Scalar},
            {"type", {}, talas::expr::ValueDomain::Scalar, "power", {"power", "linear"}},
        },
        descriptor.typeId);
    require(
        descriptor.inputs[2].signal.rate
                == talas::core::PortSignalRate::Sample
            && descriptor.inputs[3].signal.rate
                == talas::core::PortSignalRate::Sample
            && descriptor.inputs[4].signal.rate
                == talas::core::PortSignalRate::Sample
            && descriptor.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !descriptor.capabilities.supportsReset
            && descriptor.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && std::ranges::find(
                descriptor.capabilities.limitations,
                "allocation-free-dual-output-whole-block-failure-atomic-retry")
                != descriptor.capabilities.limitations.end(),
        "C135 Crossfade should expose supported checked sample-rate aligned stereo authority");
}

void testStereoMix2DescriptorDeclaresExactContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.stereoMix2");

    requirePorts(
        descriptor.inputs,
        {
            {"in1", talas::core::PortType::Audio, true},
            {"in2", talas::core::PortType::Audio, true},
            {"left", talas::core::PortType::Control, false},
            {"right", talas::core::PortType::Control, false},
            {"pan1", talas::core::PortType::Control, false},
            {"scale1", talas::core::PortType::Control, false},
            {"pan2", talas::core::PortType::Control, false},
            {"scale2", talas::core::PortType::Control, false},
        },
        descriptor.typeId,
        "input");
    requirePorts(
        descriptor.outputs,
        {
            {"leftOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
        },
        descriptor.typeId,
        "output");
    requireParameters(
        descriptor.parameters,
        {
            {"left", "1.0", talas::expr::ValueDomain::Scalar},
            {"right", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan1", "0.0", talas::expr::ValueDomain::Scalar},
            {"scale1", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan2", "1.0", talas::expr::ValueDomain::Scalar},
            {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
        },
        descriptor.typeId);
    require(
        std::ranges::all_of(
            std::span{descriptor.inputs}.subspan(2),
            [](const auto& port) {
                return port.signal.rate
                    == talas::core::PortSignalRate::Sample;
            })
            && descriptor.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !descriptor.capabilities.supportsReset
            && descriptor.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && std::ranges::find(
                descriptor.capabilities.limitations,
                "allocation-free-dual-output-whole-block-failure-atomic-retry")
                != descriptor.capabilities.limitations.end(),
        "C136 Stereo Mix 2 should expose supported checked sample-rate aligned stereo authority");
}

void testStereoMix4DescriptorDeclaresExactContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.stereoMix4");

    requirePorts(
        descriptor.inputs,
        {
            {"in1", talas::core::PortType::Audio, true},
            {"in2", talas::core::PortType::Audio, true},
            {"in3", talas::core::PortType::Audio, true},
            {"in4", talas::core::PortType::Audio, true},
            {"left", talas::core::PortType::Control, false},
            {"right", talas::core::PortType::Control, false},
            {"pan1", talas::core::PortType::Control, false},
            {"scale1", talas::core::PortType::Control, false},
            {"pan2", talas::core::PortType::Control, false},
            {"scale2", talas::core::PortType::Control, false},
            {"pan3", talas::core::PortType::Control, false},
            {"scale3", talas::core::PortType::Control, false},
            {"pan4", talas::core::PortType::Control, false},
            {"scale4", talas::core::PortType::Control, false},
        },
        descriptor.typeId,
        "input");
    requirePorts(
        descriptor.outputs,
        {
            {"leftOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
        },
        descriptor.typeId,
        "output");
    requireParameters(
        descriptor.parameters,
        {
            {"left", "1.0", talas::expr::ValueDomain::Scalar},
            {"right", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan1", "0.0", talas::expr::ValueDomain::Scalar},
            {"scale1", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan2", "0.33", talas::expr::ValueDomain::Scalar},
            {"scale2", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan3", "0.67", talas::expr::ValueDomain::Scalar},
            {"scale3", "1.0", talas::expr::ValueDomain::Scalar},
            {"pan4", "1.0", talas::expr::ValueDomain::Scalar},
            {"scale4", "1.0", talas::expr::ValueDomain::Scalar},
        },
        descriptor.typeId);
    require(
        std::ranges::all_of(
            std::span{descriptor.inputs}.subspan(4),
            [](const auto& port) {
                return port.signal.rate
                    == talas::core::PortSignalRate::Sample;
            })
            && descriptor.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && descriptor.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && descriptor.capabilities.state
                == talas::core::NodeStateModel::Stateless
            && !descriptor.capabilities.supportsReset
            && descriptor.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && descriptor.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && descriptor.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && descriptor.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && std::ranges::find(
                descriptor.capabilities.limitations,
                "allocation-free-dual-output-whole-block-failure-atomic-retry")
                != descriptor.capabilities.limitations.end(),
        "C137 Stereo Mix 4 should expose supported checked sample-rate aligned stereo authority");
}

void testOscilloscopeDisplayDescriptorDeclaresPassthroughContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.oscilloscopeDisplay");

    requirePorts(
        descriptor.inputs,
        {
            {"audioIn", talas::core::PortType::Audio, true},
            {"rightIn", talas::core::PortType::Audio, false},
            {"trigger", talas::core::PortType::Event, false},
            {"sweepOrTrig", talas::core::PortType::Audio, false},
            {"level", talas::core::PortType::Control, false},
        },
        descriptor.typeId,
        "input");
    requirePorts(descriptor.outputs,
        {
            {"audioOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
            {"sweepOrTrigOut", talas::core::PortType::Audio, false},
        }, descriptor.typeId, "output");
    requireParameters(descriptor.parameters,
        {
            {"sweepFreq", "30 hz", talas::expr::ValueDomain::Hertz},
            {"initialSweepFrequency", "30 hz", talas::expr::ValueDomain::Hertz},
            {"level", "1.0", talas::expr::ValueDomain::Scalar},
            {"trigSwap", "", talas::expr::ValueDomain::Scalar,
                "", {}, false},
            {"sweepOrTrig", "", talas::expr::ValueDomain::Scalar,
                "sweep", {"sweep", "trigger"}},
            {"trigThresh", "", talas::expr::ValueDomain::Scalar,
                "rising", {"rising", "falling"}},
        },
        descriptor.typeId);
    require(descriptor.capabilities.outputBusLayouts
            == std::vector{talas::core::AudioBusLayoutKind::Mono,
                talas::core::AudioBusLayoutKind::Stereo},
        "OscilloscopeDisplay should advertise its ordered mono and stereo outputs");
}

void testSpectrumAnalyzerDisplayDescriptorDeclaresPassthroughContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& descriptor = requireNodeType(registry, "talas.spectrumAnalyzerDisplay");

    requirePorts(descriptor.inputs, {{"audioIn", talas::core::PortType::Audio, true}}, descriptor.typeId, "input");
    requirePorts(descriptor.outputs, {{"audioOut", talas::core::PortType::Audio, false}}, descriptor.typeId, "output");
    requireParameters(descriptor.parameters, {}, descriptor.typeId);
}

void testAnnotationDescriptorDeclaresPassThroughContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const ExpectedNodeDescriptor expected{
        "talas.annotation",
        "Annotation",
        "Annotation",
        {{"audioIn", talas::core::PortType::Audio, true}},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {{"text", {}, talas::expr::ValueDomain::Scalar, {}, {}, {}, {}, {}, talas::core::ParameterKind::Text}},
    };

    const auto& descriptor = requireNodeType(registry, expected.typeId);
    require(descriptor.displayName == expected.displayName, expected.typeId + " should declare expected display name");
    require(descriptor.category == expected.category, expected.typeId + " should declare expected category");
    requirePorts(descriptor.inputs, expected.inputs, expected.typeId, "input");
    requirePorts(descriptor.outputs, expected.outputs, expected.typeId, "output");
    requireParameters(descriptor.parameters, expected.parameters, expected.typeId);
}

void testBuiltinParametersDeclareExpectedExpressionDomains()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();

    requirePorts(
        requireNodeType(registry, "talas.oscillatorTimeIndex").inputs,
        {{"frequency", talas::core::PortType::Audio, false}},
        "talas.oscillatorTimeIndex", "input");
    requireParameters(
        requireNodeType(registry, "talas.oscillatorTimeIndex").parameters,
        {{"frequency", "440 hz", talas::expr::ValueDomain::Hertz}},
        "talas.oscillatorTimeIndex");
    requirePorts(
        requireNodeType(registry, "talas.saturator").inputs,
        {
            {"audioIn", talas::core::PortType::Audio, true},
            {"rightIn", talas::core::PortType::Audio, false},
            {"channel3In", talas::core::PortType::Audio, false},
            {"channel4In", talas::core::PortType::Audio, false},
            {"threshold", talas::core::PortType::Audio, false},
        },
        "talas.saturator", "input");
    requirePorts(
        requireNodeType(registry, "talas.saturator").outputs,
        {
            {"audioOut", talas::core::PortType::Audio, false},
            {"rightOut", talas::core::PortType::Audio, false},
            {"channel3Out", talas::core::PortType::Audio, false},
            {"channel4Out", talas::core::PortType::Audio, false},
        },
        "talas.saturator", "output");
    requireParameters(
        requireNodeType(registry, "talas.saturator").parameters,
        {
            {"threshold", "0.25", talas::expr::ValueDomain::Scalar},
            {"transitionWidth", "0.1", talas::expr::ValueDomain::Scalar},
            {"mode", {}, talas::expr::ValueDomain::Scalar,
                "saturator", {"saturator", "repressor"}},
        },
        "talas.saturator");
    requirePorts(
        requireNodeType(registry, "talas.oscillator").inputs,
        {
            {"frequency", talas::core::PortType::Control, false},
            {"amplitude", talas::core::PortType::Control, false},
            {"envelope", talas::core::PortType::Control, false},
            {"modulator", talas::core::PortType::Audio, false},
            {"pitchBend", talas::core::PortType::Control, false},
            {"reset", talas::core::PortType::Control, false},
        },
        "talas.oscillator",
        "input");
    require(
        requireParameter(registry, "talas.oscillator", "frequency").domain == talas::expr::ValueDomain::Hertz,
        "talas.oscillator.frequency should declare Hertz expressions");
    require(
        requireParameter(registry, "talas.oscillator", "amplitude").domain == talas::expr::ValueDomain::Scalar,
        "talas.oscillator.amplitude should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.oscillator", "envelope").domain == talas::expr::ValueDomain::Scalar,
        "talas.oscillator.envelope should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.oscillator", "modulation").kind == talas::core::ParameterKind::Text,
        "talas.oscillator.modulation should declare a text selector");
    require(
        requireParameter(registry, "talas.oscillator", "maxMI").domain == talas::expr::ValueDomain::Scalar,
        "talas.oscillator.maxMI should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.oscillator", "pitchBend").domain == talas::expr::ValueDomain::Scalar,
        "talas.oscillator.pitchBend should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.oscillator", "reset").domain == talas::expr::ValueDomain::Scalar,
        "talas.oscillator.reset should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.oscillator", "wavetable").kind == talas::core::ParameterKind::Resource,
        "talas.oscillator.wavetable should declare a resource parameter");
    require(
        requireParameter(registry, "talas.oscillator", "interpolation").choices == std::vector<std::string>({"linear", "none"}),
        "talas.oscillator.interpolation should declare linear/none choices");
    require(
        requireParameter(registry, "talas.oscillator", "tableLength").choices
                == std::vector<std::string>({"first4096", "legacyWholeTable"})
            && requireParameter(registry, "talas.oscillator", "tableLength").defaultChoice
                == "first4096",
        "talas.oscillator.tableLength should read the first 4096 entries by default");
    require(
        requireParameter(registry, "talas.constant", "value").domain == talas::expr::ValueDomain::Scalar,
        "talas.constant.value should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.vca", "gain").domain == talas::expr::ValueDomain::Scalar,
        "talas.vca.gain should declare Scalar expressions");
    requirePorts(
        requireNodeType(registry, "talas.linearRamp").inputs,
        {
            {"duration", talas::core::PortType::Control, false},
            {"start", talas::core::PortType::Control, false},
            {"end", talas::core::PortType::Control, false},
        },
        "talas.linearRamp",
        "input");
    requirePorts(
        requireNodeType(registry, "talas.ramp").inputs,
        {
            {"trigger", talas::core::PortType::Control, false},
            {"duration", talas::core::PortType::Control, false},
        },
        "talas.ramp",
        "input");
    require(
        requireParameter(registry, "talas.ramp", "trigger").domain == talas::expr::ValueDomain::Scalar,
        "talas.ramp.trigger should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.ramp", "duration").domain == talas::expr::ValueDomain::Seconds,
        "talas.ramp.duration should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.ramp", "variant").choices
            == std::vector<std::string>({"ramp", "fullRamp", "repeatingRamp", "repeatingFullRamp"}),
        "talas.ramp.variant should declare the four Ramp family choices");
    require(requireParameter(registry, "talas.ramp", "trigger").defaultExpression == "1.0", "talas.ramp.trigger should default to 1");
    require(requireParameter(registry, "talas.ramp", "duration").defaultExpression == "1 s", "talas.ramp.duration should default to one second");
    require(requireParameter(registry, "talas.ramp", "variant").defaultChoice == "ramp", "talas.ramp.variant should default to ramp");
    requirePorts(
        requireNodeType(registry, "talas.lfo").inputs,
        {
            {"frequency", talas::core::PortType::Control, false},
            {"minimum", talas::core::PortType::Control, false},
            {"maximum", talas::core::PortType::Control, false},
        },
        "talas.lfo",
        "input");
    require(
        requireParameter(registry, "talas.delayWithFeedback", "delay").domain == talas::expr::ValueDomain::Seconds,
        "talas.delayWithFeedback.delay should declare Seconds expressions");
    requirePorts(
        requireNodeType(registry, "talas.delayWithFeedback").inputs,
        {
            {"audioIn", talas::core::PortType::Audio, true},
            {"delay", talas::core::PortType::Control, false},
            {"feedback", talas::core::PortType::Control, false},
            {"mix", talas::core::PortType::Control, false},
            {"scale", talas::core::PortType::Control, false},
            {"delayScale", talas::core::PortType::Control, false},
            {"rightIn", talas::core::PortType::Audio, false},
        },
        "talas.delayWithFeedback",
        "input");
    require(
        requireParameter(registry, "talas.phaseShiftBy90", "frequency").domain == talas::expr::ValueDomain::Hertz,
        "talas.phaseShiftBy90.frequency should declare Hertz expressions");
    require(
        requireParameter(registry, "talas.setRange", "oldMin").domain == talas::expr::ValueDomain::Scalar,
        "talas.setRange.oldMin should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.setRange", "oldMax").domain == talas::expr::ValueDomain::Scalar,
        "talas.setRange.oldMax should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.setRange", "newMin").domain == talas::expr::ValueDomain::Scalar,
        "talas.setRange.newMin should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.setRange", "newMax").domain == talas::expr::ValueDomain::Scalar,
        "talas.setRange.newMax should declare Scalar expressions");
    require(
        requireParameter(registry, "talas.amplitudeFollower", "timeConstant").domain == talas::expr::ValueDomain::Seconds,
        "talas.amplitudeFollower.timeConstant should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.frequencyTracker", "minFrequency").domain == talas::expr::ValueDomain::Hertz,
        "talas.frequencyTracker.minFrequency should declare Hertz expressions");
    require(
        requireParameter(registry, "talas.frequencyTracker", "maxFrequency").domain == talas::expr::ValueDomain::Hertz,
        "talas.frequencyTracker.maxFrequency should declare Hertz expressions");
    require(
        requireParameter(registry, "talas.peakDetector", "attackTime").domain == talas::expr::ValueDomain::Seconds,
        "talas.peakDetector.attackTime should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.peakDetector", "releaseTime").domain == talas::expr::ValueDomain::Seconds,
        "talas.peakDetector.releaseTime should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.setDuration", "duration").domain == talas::expr::ValueDomain::Seconds,
        "talas.setDuration.duration should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.setDuration", "startTime").domain == talas::expr::ValueDomain::Seconds,
        "talas.setDuration.startTime should declare Seconds expressions");
    require(
        requireParameter(registry, "talas.timeOffset", "offset").domain == talas::expr::ValueDomain::Seconds,
        "talas.timeOffset.offset should declare Seconds expressions");
}

// A Sound is a source, with no inputs (Noise,
// Oscillator, GenericSource, FormantBankOscillator, SumOfSines, DiskPlayer
// and Sample), a modifier of the Sound at its input (Filter, Vocoder,
// DelayWithFeedback, DiskRecorder; its TimeFrequencyScaler reads a sample,
// as its prototype does) or a combiner of several (a Script).
// The optional ports are Sounds pasted into fields, not Inputs, and
// a FormantBankOscillator's CascadeInput is a Sound added to its own
//.
void testSoundsAreSourcesModifiersAndCombiners()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto type = [&](std::string_view typeId) {
        const auto* found = registry.findNodeType(typeId);
        require(found != nullptr, std::string{typeId} + " should be in the catalog");
        return found;
    };
    for (const auto* source : {"talas.noise", "talas.oscillator", "talas.genericSource", "talas.formantBankOscillator",
             "talas.sumOfSines", "talas.diskPlayer", "talas.sample"})
    {
        require(std::ranges::none_of(type(source)->inputs, [](const talas::core::PortDescriptor& port) {
                    return port.required && port.id != "cascadeInput";
                }),
            std::string{""} + source + " should be a source, with no inputs");
    }
    for (const auto* modifier : {"talas.filter", "talas.vocoder", "talas.delayWithFeedback", "talas.diskRecorder"})
    {
        require(std::ranges::any_of(type(modifier)->inputs, [](const talas::core::PortDescriptor& port) {
                    return port.type == talas::core::PortType::Audio && port.required && !port.acceptsMany;
                }),
            std::string{""} + modifier + " should modify the Sound at its input");
    }
    const auto* script = type("talas.script");
    require(std::ranges::any_of(script->inputs, [](const talas::core::PortDescriptor& port) {
                   return port.type == talas::core::PortType::Audio && port.acceptsMany;
               }),
        "a Script should combine several Inputs");
}

void testSmallAudioGraphValidatesAgainstBuiltinRegistry()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::core::Graph graph;
    graph.addNode({"osc1", "talas.oscillator"});
    graph.addNode({"vca1", "talas.vca"});
    graph.addNode({"out1", "talas.audioOutput"});
    graph.connect({"osc1", "audioOut", "vca1", "audioIn"});
    graph.connect({"vca1", "audioOut", "out1", "audioIn"});

    const auto result = graph.validate(registry);
    require(result.ok(), "oscillator -> vca -> audioOutput should validate with the built-in registry");
}

void testRegisteringBuiltinsTwiceThrows()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();

    bool rejected = false;
    try
    {
        talas::core::registerBuiltinNodeTypes(registry);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }

    require(rejected, "registering built-ins twice into one registry should throw std::invalid_argument");
}

void testThresholdDescriptorDeclaresStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& threshold = requireNodeType(registry, "talas.threshold");
    require(
        threshold.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && threshold.capabilities.supportsReset
            && !threshold.capabilities.transportAware
            && !threshold.capabilities.typedEvents
            && threshold.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && threshold.capabilities.latency.kind
                == talas::core::LatencyKind::None,
        "C66 Threshold should publish its exact session-stateful reset contract");
}

void testReverbSectionDescriptorDeclaresBoundedPrivateMemoryContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& reverb = requireNodeType(registry, "talas.reverbSection");
    require(
        reverb.supportLevel == talas::core::NodeSupportLevel::FullRuntime
            && reverb.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && reverb.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && reverb.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && reverb.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && reverb.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && reverb.capabilities.supportsReset
            && !reverb.capabilities.transportAware
            && !reverb.capabilities.typedEvents
            && reverb.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && reverb.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && reverb.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && reverb.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                reverb.capabilities.limitations,
                "sample-progressive-partition-stable-zero-delay-history-and-transport-reset")
                != reverb.capabilities.limitations.end()
            && std::ranges::find(
                reverb.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-and-buffer-authority")
                != reverb.capabilities.limitations.end(),
        "C141 Reverb Section should publish its exact bounded private-memory contract");
}

void testMonotonizerDescriptorDeclaresKnownPitchStateLatencyAndSubsetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& monotonizer = requireNodeType(registry, "talas.monotonizer");
    require(
        monotonizer.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && monotonizer.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && monotonizer.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && monotonizer.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && monotonizer.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && monotonizer.capabilities.supportsReset
            && !monotonizer.capabilities.transportAware
            && !monotonizer.capabilities.typedEvents
            && monotonizer.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && monotonizer.capabilities.latency.kind
                == talas::core::LatencyKind::PreparationTime
            && monotonizer.capabilities.latency.parameterId
                == "frequency"
            && monotonizer.capabilities.latency.preparedRule
                == talas::core::PreparedLatencyRule::MonotonizerProfile
            && monotonizer.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && monotonizer.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                monotonizer.capabilities.limitations,
                "offline-and-prepared-known-pitch-retargeting-or-range-pitch-tracking-with-zero-or-1023-frame-latency")
                != monotonizer.capabilities.limitations.end()
            && std::ranges::find(
                monotonizer.capabilities.limitations,
                "scale-quantization-stereo-multichannel-and-exact-reference-monotonizer-parity-unsupported")
                != monotonizer.capabilities.limitations.end(),
        "C105 Monotonizer should publish exact known-pitch and tracked-range state, latency, layout, and subset truth");
}

void testRunningMaxDescriptorDeclaresFiniteStateResetAndLatencyContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& runningMax = requireNodeType(registry, "talas.runningMax");
    require(
        runningMax.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && runningMax.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && runningMax.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && runningMax.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && runningMax.capabilities.supportsReset
            && !runningMax.capabilities.transportAware
            && !runningMax.capabilities.typedEvents
            && runningMax.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && runningMax.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && runningMax.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && runningMax.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                runningMax.capabilities.limitations,
                "finite-control-session-running-maximum-with-positive-zero-reset-baseline")
                != runningMax.capabilities.limitations.end()
            && std::ranges::find(
                runningMax.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-finite-reset-before-current-value")
                != runningMax.capabilities.limitations.end()
            && std::ranges::find(
                runningMax.capabilities.limitations,
                "connected-control-precedence-and-sample-accurate-direct-event-value-scheduling")
                != runningMax.capabilities.limitations.end()
            && std::ranges::find(
                runningMax.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != runningMax.capabilities.limitations.end(),
        "C151 Running Max should publish its exact scheduled reset authority contract");
}

void testRunningMinDescriptorDeclaresFiniteStateResetAndLatencyContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& runningMin = requireNodeType(registry, "talas.runningMin");
    require(
        runningMin.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && runningMin.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && runningMin.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && runningMin.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && runningMin.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && runningMin.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && runningMin.capabilities.supportsReset
            && !runningMin.capabilities.transportAware
            && !runningMin.capabilities.typedEvents
            && runningMin.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && runningMin.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && runningMin.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && runningMin.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                runningMin.capabilities.limitations,
                "finite-control-session-running-minimum-with-selected-maximum-amplitude-reset-baseline")
                != runningMin.capabilities.limitations.end()
            && std::ranges::find(
                runningMin.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-finite-reset-and-maximum-amplitude-before-current-value")
                != runningMin.capabilities.limitations.end()
            && std::ranges::find(
                runningMin.capabilities.limitations,
                "independent-connected-control-precedence-and-sample-accurate-direct-event-value-scheduling")
                != runningMin.capabilities.limitations.end()
            && std::ranges::find(
                runningMin.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != runningMin.capabilities.limitations.end(),
        "C152 Running Min should publish its exact paired scheduled-control authority contract");
}

void testRmsSquaredDescriptorDeclaresFiniteEnvelopeStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& rmsSquared = requireNodeType(registry, "talas.rmsSquared");
    require(
        rmsSquared.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && rmsSquared.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && rmsSquared.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && rmsSquared.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && rmsSquared.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && rmsSquared.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && rmsSquared.capabilities.supportsReset
            && !rmsSquared.capabilities.transportAware
            && !rmsSquared.capabilities.typedEvents
            && rmsSquared.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && rmsSquared.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && rmsSquared.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && rmsSquared.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "mono-finite-exponential-squared-envelope-with-established-zero-point-four-time-constant-calibration")
                != rmsSquared.capabilities.limitations.end()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-positive-seconds-time-constant-and-non-negative-scalar-scale")
                != rmsSquared.capabilities.limitations.end()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "connected-control-precedence-and-sample-accurate-direct-event-value-scheduling")
                != rmsSquared.capabilities.limitations.end()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != rmsSquared.capabilities.limitations.end()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset")
                != rmsSquared.capabilities.limitations.end()
            && std::ranges::find(
                rmsSquared.capabilities.limitations,
                "true-rms-multichannel-arbitrary-window-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported")
                != rmsSquared.capabilities.limitations.end(),
        "C150 RMS Squared should publish its exact scheduled envelope authority contract");
}

void testAmplitudeFollowerDescriptorDeclaresFiniteEnvelopeStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& follower = requireNodeType(registry, "talas.amplitudeFollower");
    require(
        follower.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && follower.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && follower.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && follower.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && follower.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && follower.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && follower.capabilities.supportsReset
            && !follower.capabilities.transportAware
            && !follower.capabilities.typedEvents
            && follower.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && follower.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && follower.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && follower.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                follower.capabilities.limitations,
                "mono-finite-absolute-amplitude-envelope-with-established-zero-point-four-time-constant-calibration")
                != follower.capabilities.limitations.end()
            && std::ranges::find(
                follower.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-positive-seconds-time-constant-and-non-negative-scalar-scale")
                != follower.capabilities.limitations.end()
            && std::ranges::find(
                follower.capabilities.limitations,
                "connected-control-precedence-and-sample-accurate-direct-event-value-scheduling")
                != follower.capabilities.limitations.end()
            && std::ranges::find(
                follower.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != follower.capabilities.limitations.end()
            && std::ranges::find(
                follower.capabilities.limitations,
                "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset")
                != follower.capabilities.limitations.end()
            && std::ranges::find(
                follower.capabilities.limitations,
                "multichannel-independent-attack-release-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported")
                != follower.capabilities.limitations.end(),
        "C148 Amplitude Follower should publish its exact scheduled finite-envelope authority contract");
}

void testPeakDetectorDescriptorDeclaresFiniteAsymmetricEnvelopeContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& detector = requireNodeType(registry, "talas.peakDetector");
    require(
        detector.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && detector.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && detector.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && detector.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && detector.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && detector.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && detector.capabilities.supportsReset
            && !detector.capabilities.transportAware
            && !detector.capabilities.typedEvents
            && detector.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && detector.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && detector.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && detector.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                detector.capabilities.limitations,
                "mono-finite-asymmetric-peak-envelope-with-strict-scaled-amplitude-branch-selection")
                != detector.capabilities.limitations.end()
            && std::ranges::find(
                detector.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-positive-seconds-attack-release-and-non-negative-scalar-scale")
                != detector.capabilities.limitations.end()
            && std::ranges::find(
                detector.capabilities.limitations,
                "connected-control-precedence-and-sample-accurate-direct-event-value-scheduling")
                != detector.capabilities.limitations.end()
            && std::ranges::find(
                detector.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != detector.capabilities.limitations.end()
            && std::ranges::find(
                detector.capabilities.limitations,
                "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset")
                != detector.capabilities.limitations.end()
            && std::ranges::find(
                detector.capabilities.limitations,
                "multichannel-arbitrary-window-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported")
                != detector.capabilities.limitations.end(),
        "C149 Peak Detector should publish its exact scheduled asymmetric-envelope authority contract");
}

void testFrequencyTrackerDescriptorDeclaresBoundedHistoryContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& tracker = requireNodeType(registry, "talas.frequencyTracker");
    require(
        tracker.capabilities.offline
                == talas::core::ExecutionSupport::Subset
            && tracker.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && tracker.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && tracker.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && tracker.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && tracker.capabilities.supportsReset
            && !tracker.capabilities.transportAware
            && !tracker.capabilities.typedEvents
            && tracker.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && tracker.capabilities.latency.kind
                == talas::core::LatencyKind::VariableOrUnknown
            && tracker.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && tracker.capabilities.outputBusLayouts.empty()
            && tracker.capabilities.limitations
                == std::vector<std::string>{
                    "offline-functional-subset",
                    "offline-and-prepared-shared-checked-finite-monophonic-yin-callback-window-tracker",
                    "static-or-connected-bounded-min-max-confidence-scale-default-detectors-and-static-emphasis-profile",
                    "bounded-preallocated-history-variable-latency-align-zero-latency-audio-ancestry-authority",
                    "compiler-owned-exact-descriptor-plan-settings-empty-resource-bundle-parameter-input-producer-source-buffer-output-scope-state-reset-variable-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-and-offline-render-failure-atomic-output-history-previous-input-control-retry-reset-and-zero-frame-no-op",
                    "sample-progressive-partition-invariant-polyphonic-non-default-detector-dynamic-emphasis-multichannel-typed-event-and-exact-reference-semantics-unsupported",
                },
        "C197 Frequency Tracker should publish its exact Supported bounded retained-history authority contract");
}

void testArDescriptorDeclaresFiniteLinearEnvelopeContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& envelope = requireNodeType(registry, "talas.ar");
    require(
        envelope.capabilities.offline == talas::core::ExecutionSupport::Supported
            && envelope.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && envelope.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && envelope.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && envelope.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && envelope.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && envelope.capabilities.supportsReset
            && !envelope.capabilities.transportAware
            && !envelope.capabilities.typedEvents
            && envelope.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && envelope.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && envelope.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && envelope.capabilities.outputBusLayouts.empty()
            && envelope.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-finite-linear-or-exponential-ar-envelope",
                    "static-or-direct-event-value-positive-attack-release-finite-scale-and-gate-with-connected-gate-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-live-times-scale-and-current-value-retrigger",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset",
                    "connected-time-scale-targeted-events-polyphony-and-exact-reference-semantics-unsupported",
                },
        "C163 AR should publish its exact Full Runtime finite linear envelope contract");
}

void testAnalysisFilterDescriptorDeclaresScheduledQuadratureContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& filter = requireNodeType(registry, "talas.analysisFilter");
    require(
        filter.capabilities.offline == talas::core::ExecutionSupport::Supported
            && filter.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && filter.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && filter.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && filter.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && filter.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && filter.capabilities.supportsReset
            && !filter.capabilities.transportAware
            && !filter.capabilities.typedEvents
            && filter.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && filter.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && filter.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && filter.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && filter.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-one-period-harmonic-window-quadrature-with-format-53-legacy-bandpass",
                    "static-or-direct-event-value-positive-fundamental-and-nonnegative-harmonic-with-connected-input-precedence",
                    "sample-accurate-direct-event-value-scheduling-with-window-resynchronized-on-fundamental-or-harmonic-change",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-stereo-state-retry-and-transport-reset",
                    "exact-reference-multiband-live-tracking-arbitrary-expression-and-polyphonic-semantics-unsupported",
                },
        "C164 Analysis Filter should publish its exact Full Runtime scheduled quadrature contract");
}

void testAdsrDescriptorDeclaresFiniteTypedLinearEnvelopeContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& envelope = requireNodeType(registry, "talas.adsr");
    require(
        envelope.capabilities.offline == talas::core::ExecutionSupport::Supported
            && envelope.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && envelope.capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && envelope.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel
            && envelope.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && envelope.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && envelope.capabilities.supportsReset
            && !envelope.capabilities.transportAware
            && envelope.capabilities.typedEvents
            && envelope.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && envelope.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && envelope.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && envelope.capabilities.outputBusLayouts.empty()
            && envelope.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-finite-linear-or-exponential-adsr-kernel",
                    "static-scalar-event-value-positive-times-unit-sustain-finite-scale-gate-and-legato-with-connected-gate-precedence",
                    "sample-accurate-direct-event-value-and-ordered-targeted-typed-gate-scheduling",
                    "compiler-owned-exact-descriptor-kernel-plan-settings-resource-free-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-event-state-retry-and-transport-reset",
                    "arbitrary-curve-polyphony-connected-envelope-controls-project-authored-targeted-events-and-exact-reference-semantics-unsupported",
                },
        "C159 ADSR should publish its exact Full Runtime typed linear kernel contract");
}

void testPulseTrainDescriptorDeclaresFinitePhaseStateContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& pulse = requireNodeType(registry, "talas.pulseTrain");
    require(
        pulse.capabilities.offline == talas::core::ExecutionSupport::Supported
            && pulse.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && pulse.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && pulse.capabilities.supportsReset
            && !pulse.capabilities.transportAware
            && !pulse.capabilities.typedEvents
            && pulse.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && pulse.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && pulse.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && pulse.capabilities.outputBusLayouts.empty()
            && pulse.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-matched-checked-finite-absolute-elapsed-time-pulse-train",
                    "static-direct-event-value-or-per-frame-connected-positive-period-selected-bounded-duty-and-finite-gate",
                    "exact-pre-increment-pulse-post-increment-fold-gate-close-positive-zero-reset-and-immediate-reopen",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-elapsed-schedule-output-retry-and-reset",
                    "band-limited-phase-offset-sync-tempo-typed-trigger-multichannel-and-exact-reference-pulse-edge-semantics-unsupported",
                },
        "C183 Pulse Train should publish its exact promoted authority contract");
}

void testOscillatorDescriptorDeclaresFinitePhaseAndWavetableContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& oscillator = requireNodeType(registry, "talas.oscillator");
    require(
        oscillator.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && oscillator.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && oscillator.capabilities.offlineBinding
                == talas::core::ExecutionBinding::Kernel
            && oscillator.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::Kernel
            && oscillator.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && oscillator.capabilities.supportsReset
            && !oscillator.capabilities.transportAware
            && !oscillator.capabilities.typedEvents
            && oscillator.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && oscillator.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && oscillator.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && oscillator.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && oscillator.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-finite-sine-or-immutable-cyclic-wavetable-oscillator-kernel",
                    "static-or-direct-event-value-or-connected-selected-finite-frequency-amplitude-envelope-pitch-bend-reset-and-audio-rate-fm-that-wraps-the-phase-and-aliases-past-nyquist",
                    "sample-accurate-direct-event-value-scheduling-with-positive-reset-before-current-sample-linear-or-nearest-wavetable-and-phase-continuity",
                    "compiler-owned-exact-descriptor-kernel-plan-settings-immutable-resource-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-state-upstream-schedule-retry-and-transport-reset",
                    "exact-reference-band-limiting-negative-frequency-file-backed-and-metadata-wavetables-additional-interpolation-phase-sync-typed-trigger-multichannel-polyphony-and-proprietary-semantics-unsupported",
                },
        "C160 Oscillator should publish its exact bounded promoted contract");
}

void testLfoDescriptorDeclaresFinitePhaseAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& lfo = requireNodeType(registry, "talas.lfo");
    require(
        lfo.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && lfo.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && lfo.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && lfo.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && lfo.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && lfo.capabilities.supportsReset
            && !lfo.capabilities.transportAware
            && !lfo.capabilities.typedEvents
            && lfo.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && lfo.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && lfo.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && lfo.capabilities.outputBusLayouts.empty()
            && lfo.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-matched-checked-finite-sine-lfo",
                    "static-direct-event-value-or-per-frame-connected-selected-non-negative-frequency-and-finite-range-endpoints",
                    "exact-radians-pre-increment-sampling-phase-continuity-positive-zero-transport-reset-and-historical-arithmetic-order",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-phase-schedule-output-retry-and-reset",
                    "negative-frequency-alternate-waveforms-explicit-reset-phase-modulation-sync-tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported",
                },
        "C182 LFO should publish its exact bounded promoted contract");
}

void testRampDescriptorDeclaresFiniteTriggerStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& ramp = requireNodeType(registry, "talas.ramp");
    require(
        ramp.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && ramp.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && ramp.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ramp.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ramp.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && ramp.capabilities.supportsReset
            && !ramp.capabilities.transportAware
            && !ramp.capabilities.typedEvents
            && ramp.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && ramp.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && ramp.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && ramp.capabilities.outputBusLayouts.empty()
            && ramp.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-matched-checked-finite-edge-triggered-ramp",
                    "static-direct-event-value-or-per-frame-connected-selected-finite-trigger-and-positive-renderable-edge-captured-duration",
                    "exact-four-variant-positive-edge-rearm-pre-increment-endpoint-hold-and-repeat-boundary-lifecycle",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-state-schedule-output-retry-and-reset",
                    "proprietary-curve-zero-negative-duration-explicit-reset-sync-phase-tempo-typed-trigger-multichannel-and-exact-reference-semantics-unsupported",
                },
        "C184 Ramp should publish its exact promoted trigger-lifecycle contract");
}

void testSampleAndHoldDescriptorDeclaresFiniteClockStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& hold = requireNodeType(registry, "talas.sampleAndHold");
    require(
        hold.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && hold.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && hold.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && hold.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && hold.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && hold.capabilities.supportsReset
            && !hold.capabilities.transportAware
            && !hold.capabilities.typedEvents
            && hold.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && hold.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && hold.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && hold.capabilities.outputBusLayouts.empty()
            && hold.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-matched-checked-finite-periodic-sample-and-hold",
                    "required-control-input-static-direct-event-value-or-per-frame-connected-positive-renderable-hold-time-and-static-non-negative-renderable-offset",
                    "exact-nearest-frame-offset-live-per-frame-modulo-period-capture-hold-and-absolute-clock-lifecycle",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-clock-held-state-schedule-output-retry-and-reset",
                    "proprietary-clock-reset-sync-phase-tempo-typed-event-interpolation-random-multichannel-and-exact-reference-semantics-unsupported",
                },
        "C185 Sample And Hold should publish its exact promoted periodic-clock contract");
}

void testSamplesFromDiskSingleStepDescriptorDeclaresFinitePlaybackContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& step = requireNodeType(
        registry, "talas.samplesFromDiskSingleStep");
    require(
        step.capabilities.offline == talas::core::ExecutionSupport::Subset
            && step.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && step.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && step.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && step.capabilities.supportsReset
            && !step.capabilities.transportAware
            && !step.capabilities.typedEvents
            && step.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && step.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && step.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && step.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(step.capabilities.limitations,
                   "offline-and-prepared-finite-immutable-table-single-step-playback")
                != step.capabilities.limitations.end()
            && std::ranges::find(step.capabilities.limitations,
                   "static-or-connected-integral-position-positive-level-trigger-and-gate-edge-restart")
                != step.capabilities.limitations.end()
            && std::ranges::find(step.capabilities.limitations,
                   "prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry")
                != step.capabilities.limitations.end()
            && std::ranges::find(step.capabilities.limitations,
                   "prepared-realtime-failure-atomic-retry-requires-locally-transactional-graph-coexistence")
                != step.capabilities.limitations.end()
            && std::ranges::find(step.capabilities.limitations,
                   "exact-reference-disk-streaming-file-decoding-multichannel-rate-conversion-looping-and-fractional-playback-unsupported")
                != step.capabilities.limitations.end(),
        "C99 Samples From Disk Single Step should publish its exact finite immutable-table playback contract");
}

void testPhaseShiftBy90DescriptorDeclaresScheduledAllpassContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& phase = requireNodeType(registry, "talas.phaseShiftBy90");
    require(
        phase.capabilities.offline == talas::core::ExecutionSupport::Supported
            && phase.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && phase.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && phase.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && phase.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && phase.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && phase.capabilities.supportsReset
            && !phase.capabilities.transportAware
            && !phase.capabilities.typedEvents
            && phase.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && phase.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && phase.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && phase.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Stereo}
            && phase.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-stereo-two-branch-allpass-phase-shift-at-target-frequency",
                    "static-or-direct-event-value-positive-frequency-strictly-below-nyquist-with-connected-input-precedence",
                    "sample-accurate-direct-event-value-frequency-scheduling",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-and-zero-latency-authority",
                    "allocation-free-prepared-callback-failure-atomic-schedule-state-retry-and-transport-reset",
                    "exact-reference-coefficients-gain-transients-nonlinearity-broadband-hilbert-arbitrary-expression-smoothing-multichannel-and-polyphony-semantics-unverified",
                },
        "C165 Phase Shift By 90 should publish its exact Full Runtime scheduled allpass contract");
}

void testSingleSideBandRmDescriptorDeclaresScheduledQuadratureContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& ssb = requireNodeType(registry, "talas.singleSideBandRm");
    require(
        ssb.capabilities.offline == talas::core::ExecutionSupport::Supported
            && ssb.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && ssb.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ssb.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && ssb.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && ssb.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && ssb.capabilities.supportsReset
            && !ssb.capabilities.transportAware
            && !ssb.capabilities.typedEvents
            && ssb.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && ssb.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && ssb.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && ssb.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && ssb.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-stereo-phase-pair-and-summed-quadrature-frequency-shift",
                    "static-or-direct-event-value-positive-frequency-below-nyquist-and-positive-frequency-scale-with-connected-input-precedence",
                    "sample-accurate-direct-event-value-frequency-and-frequency-scale-scheduling-with-signed-wrapped-quadrature-phase",
                    "compiler-owned-exact-descriptor-plan-settings-resource-free-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-schedule-two-filter-and-carrier-state-retry-and-transport-reset",
                    "exact-reference-coefficients-gain-transients-nonlinearity-broadband-behavior-frequency-scale-range-aliasing-smoothing-and-polyphony-semantics-unverified",
                },
        "C166 Single Side Band RM should publish its exact Full Runtime scheduled quadrature contract");
}

void testSosOscillatorsDescriptorDeclaresStaticSpectrumWavetableAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& sos = requireNodeType(registry, "talas.sosOscillators");
    require(
        sos.supportLevel == talas::core::NodeSupportLevel::FullRuntime
            && sos.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && sos.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && sos.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sos.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && sos.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && sos.capabilities.supportsReset
            && !sos.capabilities.transportAware
            && !sos.capabilities.typedEvents
            && sos.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::ImmutableRead
            && sos.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && sos.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && sos.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && sos.capabilities.limitations
                == std::vector<std::string>{
                    "offline-and-prepared-shared-checked-static-spectrum-sos-additive-resynthesis-or-demoted-any-spectrum-phase-continuous-bank",
                    "static-context-free-positive-integral-selected-partial-range-and-cascade-first-ordered-sum",
                    "blank-sine-or-compiler-owned-immutable-finite-cyclic-linear-wavetable-generation",
                    "compiler-owned-exact-descriptor-plan-settings-resource-parameter-binding-buffer-producer-scope-zero-latency-and-local-transaction-authority",
                    "allocation-free-prepared-callback-failure-atomic-phase-resource-retry-and-transport-reset",
                    "exact-reference-sosanalysis-file-streaming-dsp-bank-stereo-multichannel-and-polyphony-semantics-unsupported",
                },
        "C167 SOS Oscillators should publish its exact Full Runtime static Spectrum/wavetable authority");
}

void testPulseGeneratorDescriptorDeclaresFinitePhaseStateContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& pulse = requireNodeType(registry, "talas.pulseGenerator");
    require(
        pulse.capabilities.offline == talas::core::ExecutionSupport::Subset
            && pulse.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Subset
            && pulse.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && pulse.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && pulse.capabilities.supportsReset
            && !pulse.capabilities.transportAware
            && !pulse.capabilities.typedEvents
            && pulse.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && pulse.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && pulse.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && pulse.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(pulse.capabilities.limitations,
                   "offline-and-prepared-finite-zero-dc-normalized-phase-pulse-generator")
                != pulse.capabilities.limitations.end()
            && std::ranges::find(pulse.capabilities.limitations,
                   "static-or-event-value-or-connected-selected-frequency-envelope-duty-reset-and-bounded-audio-rate-fm")
                != pulse.capabilities.limitations.end()
            && std::ranges::find(pulse.capabilities.limitations,
                   "nonzero-reset-before-current-sample-zero-frequency-phase-hold-and-duty-boundary-path-distinction")
                != pulse.capabilities.limitations.end()
            && std::ranges::find(pulse.capabilities.limitations,
                   "prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry")
                != pulse.capabilities.limitations.end()
            && std::ranges::find(pulse.capabilities.limitations,
                   "prepared-realtime-failure-atomic-retry-requires-locally-transactional-graph-coexistence")
                != pulse.capabilities.limitations.end()
            && std::ranges::find(pulse.capabilities.limitations,
                   "exact-reference-aliasing-additional-interpolation-phase-sync-and-typed-trigger-semantics-unsupported")
                != pulse.capabilities.limitations.end(),
        "C94 Pulse Generator should publish its exact finite normalized-phase contract");
}

void testTimeControlDescriptorDeclaresStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& timeControl = requireNodeType(registry, "talas.timeControl");
    require(
        timeControl.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && timeControl.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && timeControl.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && timeControl.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeControl.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeControl.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && timeControl.capabilities.supportsReset
            && !timeControl.capabilities.transportAware
            && !timeControl.capabilities.typedEvents
            && timeControl.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && timeControl.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && timeControl.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && timeControl.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                timeControl.capabilities.limitations,
                "fixed-capacity-prepared-push-history-with-whole-block-append-before-linear-interpolation")
                != timeControl.capabilities.limitations.end()
            && std::ranges::find(
                timeControl.capabilities.limitations,
                "pull-based-future-sample-ownership-reverse-rate-multichannel-and-exact-reference-scheduling-unsupported")
                != timeControl.capabilities.limitations.end(),
        "C144 Time Control should publish its exact bounded ring contract");
}

void testAveragingLowPassFilterDescriptorDeclaresMovingWindowContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& filter = requireNodeType(
        registry, "talas.averagingLowPassFilter");
    require(
        filter.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && filter.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && filter.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && filter.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && filter.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && filter.capabilities.supportsReset
            && !filter.capabilities.typedEvents
            && !filter.capabilities.transportAware
            && filter.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && filter.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && filter.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && filter.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                filter.capabilities.limitations,
                "sample-progressive-partition-stable-prehistory-scaling-and-oldest-sample-fraction")
                != filter.capabilities.limitations.end()
            && std::ranges::find(
                filter.capabilities.limitations,
                "prepared-realtime-allocation-free-transport-reset-and-failure-atomic-retry")
                != filter.capabilities.limitations.end(),
        "C139 Averaging Low Pass Filter should expose its exact bounded moving-window contract");
}

void testDelayWithFeedbackDescriptorDeclaresBoundedHistoryContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& delay = requireNodeType(
        registry, "talas.delayWithFeedback");
    require(
        delay.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && delay.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && delay.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && delay.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && delay.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && delay.capabilities.supportsReset
            && !delay.capabilities.typedEvents
            && !delay.capabilities.transportAware
            && delay.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && delay.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && delay.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && delay.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                delay.capabilities.limitations,
                "sample-progressive-partition-stable-zero-delay-history-and-transport-reset")
                != delay.capabilities.limitations.end()
            && std::ranges::find(
                delay.capabilities.limitations,
                "prepared-realtime-allocation-free-and-failure-atomic-retry")
                != delay.capabilities.limitations.end(),
        "C140 Delay With Feedback should expose its exact bounded-history contract");
}

void testTimeStopperDescriptorDeclaresStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& timeStopper = requireNodeType(registry, "talas.timeStopper");
    require(
        timeStopper.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && timeStopper.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && timeStopper.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && timeStopper.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeStopper.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeStopper.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && timeStopper.capabilities.supportsReset
            && !timeStopper.capabilities.transportAware
            && !timeStopper.capabilities.typedEvents
            && timeStopper.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && timeStopper.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && timeStopper.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && timeStopper.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "C145 Time Stopper should publish its promoted bounded hold contract");
}

void testSetDurationDescriptorDeclaresBoundedWindowContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& setDuration = requireNodeType(registry, "talas.setDuration");
    require(
        setDuration.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && setDuration.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && setDuration.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && setDuration.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && setDuration.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && setDuration.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && setDuration.capabilities.supportsReset
            && !setDuration.capabilities.transportAware
            && !setDuration.capabilities.typedEvents
            && setDuration.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && setDuration.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && setDuration.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && setDuration.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono},
        "C146 Set Duration should publish its promoted bounded window contract");
}

void testTimeOffsetDescriptorDeclaresStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& timeOffset = requireNodeType(registry, "talas.timeOffset");
    require(
        timeOffset.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && timeOffset.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && timeOffset.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && timeOffset.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeOffset.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && timeOffset.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && timeOffset.capabilities.supportsReset
            && !timeOffset.capabilities.transportAware
            && !timeOffset.capabilities.typedEvents
            && timeOffset.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && timeOffset.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && timeOffset.capabilities.latency.fixedFrames == 0
            && timeOffset.capabilities.latency.parameterId.empty()
            && timeOffset.capabilities.latency.preparedRule
                == talas::core::PreparedLatencyRule::None
            && timeOffset.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && timeOffset.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                timeOffset.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-and-buffer-authority")
                != timeOffset.capabilities.limitations.end()
            && std::ranges::find(
                timeOffset.capabilities.limitations,
                "connected-dynamic-reverse-retrograde-automatic-timeline-extension-pull-ahead-end-of-stream-invention-multichannel-and-exact-reference-semantics-unsupported")
                != timeOffset.capabilities.limitations.end(),
        "C142 Time Offset should publish its exact bounded static-mode contract");
}

void testWaitUntilDescriptorDeclaresBoundedQueueContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& waitUntil = requireNodeType(registry, "talas.waitUntil");
    require(
        waitUntil.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && waitUntil.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && waitUntil.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && waitUntil.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && waitUntil.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && waitUntil.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && waitUntil.capabilities.supportsReset
            && !waitUntil.capabilities.transportAware
            && !waitUntil.capabilities.typedEvents
            && waitUntil.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && waitUntil.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && waitUntil.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::Align
            && waitUntil.capabilities.outputBusLayouts
                == std::vector{talas::core::AudioBusLayoutKind::Mono}
            && std::ranges::find(
                waitUntil.capabilities.limitations,
                "shared-dependency-delayed-input-activation-with-live-resume-and-once-per-sample-producers")
                != waitUntil.capabilities.limitations.end()
            && std::ranges::find(
                waitUntil.capabilities.limitations,
                "resume-as-sound-end-detection-complete-midi-and-expression-input-lifecycle-global-timeline-gates-multichannel-and-exact-reference-scheduling-unsupported")
                != waitUntil.capabilities.limitations.end(),
        "C143 Wait Until should publish its exact delayed Input activation contract");
}

void testTriggeredSampleAndHoldDescriptorDeclaresStateAndResetContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& hold = requireNodeType(
        registry, "talas.triggeredSampleAndHold");
    require(
        hold.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && hold.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && hold.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && hold.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && hold.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && hold.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && hold.capabilities.supportsReset
            && !hold.capabilities.transportAware
            && !hold.capabilities.typedEvents
            && hold.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::None
            && hold.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && hold.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && hold.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                hold.capabilities.limitations,
                "finite-control-session-triggered-sample-and-hold-on-inactive-to-positive-edges")
                != hold.capabilities.limitations.end()
            && std::ranges::find(
                hold.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-finite-trigger-with-connected-precedence")
                != hold.capabilities.limitations.end()
            && std::ranges::find(
                hold.capabilities.limitations,
                "sample-accurate-direct-event-value-scheduling-with-same-frame-finite-capture")
                != hold.capabilities.limitations.end()
            && std::ranges::find(
                hold.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-binding-buffer-producer-scope-and-zero-latency-authority")
                != hold.capabilities.limitations.end(),
        "C153 Triggered Sample And Hold should publish its exact scheduled-edge authority contract");
}

void testTriggeredTableReadDescriptorDeclaresStateResetAndResourceContract()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& table = requireNodeType(
        registry, "talas.triggeredTableRead");
    require(
        table.supportLevel
                == talas::core::NodeSupportLevel::FullRuntime
            && table.capabilities.offline
                == talas::core::ExecutionSupport::Supported
            && table.capabilities.offlineBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && table.capabilities.preparedRealtime
                == talas::core::ExecutionSupport::Supported
            && table.capabilities.preparedRealtimeBinding
                == talas::core::ExecutionBinding::LegacyAdapter
            && table.capabilities.state
                == talas::core::NodeStateModel::SessionStateful
            && table.capabilities.supportsReset
            && !table.capabilities.transportAware
            && !table.capabilities.typedEvents
            && table.capabilities.resourceAccess
                == talas::core::ResourceAccessMode::VariableOrUnknown
            && table.capabilities.latency.kind
                == talas::core::LatencyKind::None
            && table.capabilities.fanInLatencyPolicy
                == talas::core::FanInLatencyPolicy::NotApplicable
            && table.capabilities.outputBusLayouts.empty()
            && std::ranges::find(
                table.capabilities.limitations,
                "finite-immutable-or-live-memory-table-session-cursor-with-positive-trigger-read-and-nonpositive-hold")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "same-scope-unique-prior-bounded-mono-memory-writer-with-current-sample-visibility")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "static-scalar-event-or-per-frame-connected-finite-trigger-and-gate-with-independent-connected-precedence")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "sample-accurate-direct-event-value-scheduling-with-gate-edge-reset-before-same-frame-read")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "compiler-owned-exact-descriptor-plan-settings-resource-binding-buffer-producer-scope-and-zero-latency-authority")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "allocation-free-prepared-callback-failure-atomic-retry-and-transport-reset")
                != table.capabilities.limitations.end()
            && std::ranges::find(
                table.capabilities.limitations,
                "cross-scope-memory-multichannel-event-graph-unrestricted-dynamic-language-and-exact-reference-semantics-unsupported")
                != table.capabilities.limitations.end()
            && table.capabilities.limitations.size() == 7,
        "C154 Triggered Table Read should publish its exact immutable/live-memory scheduled-control authority contract");
}

void testAnalogSequencerGeneratedIndexNameGrammar()
{
    using talas::core::parseAnalogSequencerGeneratedIndex;
    const auto blank = parseAnalogSequencerGeneratedIndex(" \t\r\n");
    require(blank.has_value() && blank->empty(),
        "blank GeneratedIndex text should disable publication");
    const std::vector<std::pair<std::string, std::string>> valid{
        {"!Stage", "Stage"},
        {"  !_stage7  ", "_stage7"},
        {"!" + std::string(128, 'A'), std::string(128, 'A')},
    };
    for (const auto& [source, expected] : valid)
    {
        const auto parsed = parseAnalogSequencerGeneratedIndex(source);
        require(parsed.has_value() && *parsed == expected,
            "GeneratedIndex should return one bounded bare ASCII Event Value name");
    }
    for (const auto& invalid : {
             std::string{"Stage"}, std::string{"!"}, std::string{"!!Stage"},
             std::string{"!Stage Other"}, std::string{"!7Stage"},
             std::string{"!Stage-Name"}, std::string{"!"} + std::string(129, 'A'),
             std::string{"!Pitch"}, std::string{"!KeyPitch"},
             std::string{"!KeyNumber"}, std::string{"!KeyDown"},
             std::string{"!KeyVelocity"}, std::string{"!KeyTimbre"},
         })
        require(!parseAnalogSequencerGeneratedIndex(invalid).has_value(),
            "GeneratedIndex should reject malformed names and keyboard Event Values");
}
}

// Every Spectral Source makes spectra in one format,
// the one an OscillatorBank takes.
void testSpectralSourcesShareOneFormat()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* bank = registry.findNodeType("talas.oscillatorBank");
    require(bank != nullptr, "the OscillatorBank should be registered");
    const auto input = std::ranges::find(bank->inputs, std::string{"spectrum"}, &talas::core::PortDescriptor::id);
    require(input != bank->inputs.end(), "the OscillatorBank should take a spectrum");
    std::size_t sources = 0;
    for (const auto& type : registry.nodeTypes())
    {
        if (type.category != "Spectral Sources")
            continue;
        ++sources;
        const auto out = std::ranges::find(type.outputs, std::string{"spectrumOut"}, &talas::core::PortDescriptor::id);
        require(out != type.outputs.end() && out->type == talas::core::PortType::Spectrum
                && out->signal.spectralFormat == input->signal.spectralFormat && talas::core::portDescriptorsCompatible(*out, *input),
            "" + type.typeId + " should make spectra in the Spectral Sources' one format");
    }
    require(sources >= 7, "the Spectral Sources should all be checked");
}

int main()
{
    testBuiltinRegistryContainsEveryV1NodeType();
    testSpectralSourcesShareOneFormat();
#if defined(__APPLE__)
    testBuiltinRegistryFitsWorkbenchWorkerStack();
#endif
    testBuiltinDescriptorIdsAreUniquePerNode();
    testBuiltinDescriptorsUseSupportedMetadataShape();
    testBuiltinDescriptorsExposeHonestRuntimeMaturity();
    testBuiltinDescriptorsExposeOrthogonalExecutionCapabilities();
    testUnclassifiedSecondaryCapabilitiesRemainExplicitlyUnknown();
    testM3SignalBusResourceAndLatencyContractsAreTyped();
    testNativeComplexFftDescriptorsFreezePreparedContracts();
    testHistoricalPackedFftDescriptorFreezesCompatibilityContract();
    testLiveSpectralAnalysisDescriptorsFreezeNativePartialFrameBoundary();
    testMaturityUpdatesDoNotRewritePreparedRealtimeCapability();
    testCapabilityClaimsRequireImplementationBindings();
    testAnalogSequencerGeneratedIndexNameGrammar();
    testRejectedCapabilityUpdateLeavesRegistryUnchanged();
    testManualOnlyDescriptorsDeclareExactCatalogContracts();
    testExactMathDescriptorsDeclareControlContracts();
    testTimingAndTrackingDescriptorsDeclareCatalogContracts();
    testIoDescriptorsDeclareCatalogContracts();
    testPanDescriptorDeclaresExactContract();
    testCrossfadeDescriptorDeclaresExactContract();
    testStereoMix2DescriptorDeclaresExactContract();
    testStereoMix4DescriptorDeclaresExactContract();
    testOscilloscopeDisplayDescriptorDeclaresPassthroughContract();
    testSpectrumAnalyzerDisplayDescriptorDeclaresPassthroughContract();
    testAnnotationDescriptorDeclaresPassThroughContract();
    testBuiltinParametersDeclareExpectedExpressionDomains();
    testRunningMaxDescriptorDeclaresFiniteStateResetAndLatencyContract();
    testRunningMinDescriptorDeclaresFiniteStateResetAndLatencyContract();
    testRmsSquaredDescriptorDeclaresFiniteEnvelopeStateAndResetContract();
    testAmplitudeFollowerDescriptorDeclaresFiniteEnvelopeStateAndResetContract();
    testPeakDetectorDescriptorDeclaresFiniteAsymmetricEnvelopeContract();
    testFrequencyTrackerDescriptorDeclaresBoundedHistoryContract();
    testArDescriptorDeclaresFiniteLinearEnvelopeContract();
    testAnalysisFilterDescriptorDeclaresScheduledQuadratureContract();
    testAdsrDescriptorDeclaresFiniteTypedLinearEnvelopeContract();
    testOscillatorDescriptorDeclaresFinitePhaseAndWavetableContract();
    testLfoDescriptorDeclaresFinitePhaseAndResetContract();
    testRampDescriptorDeclaresFiniteTriggerStateAndResetContract();
    testSampleAndHoldDescriptorDeclaresFiniteClockStateAndResetContract();
    testSamplesFromDiskSingleStepDescriptorDeclaresFinitePlaybackContract();
    testPhaseShiftBy90DescriptorDeclaresScheduledAllpassContract();
    testSingleSideBandRmDescriptorDeclaresScheduledQuadratureContract();
    testSosOscillatorsDescriptorDeclaresStaticSpectrumWavetableAuthority();
    testPulseTrainDescriptorDeclaresFinitePhaseStateContract();
    testPulseGeneratorDescriptorDeclaresFinitePhaseStateContract();
    testReverbSectionDescriptorDeclaresBoundedPrivateMemoryContract();
    testMonotonizerDescriptorDeclaresKnownPitchStateLatencyAndSubsetContract();
    testThresholdDescriptorDeclaresStateAndResetContract();
    testTimeControlDescriptorDeclaresStateAndResetContract();
    testAveragingLowPassFilterDescriptorDeclaresMovingWindowContract();
    testDelayWithFeedbackDescriptorDeclaresBoundedHistoryContract();
    testTimeStopperDescriptorDeclaresStateAndResetContract();
    testSetDurationDescriptorDeclaresBoundedWindowContract();
    testTimeOffsetDescriptorDeclaresStateAndResetContract();
    testWaitUntilDescriptorDeclaresBoundedQueueContract();
    testTriggeredSampleAndHoldDescriptorDeclaresStateAndResetContract();
    testTriggeredTableReadDescriptorDeclaresStateResetAndResourceContract();
    testSoundsAreSourcesModifiersAndCombiners();
    testSmallAudioGraphValidatesAgainstBuiltinRegistry();
    testRegisteringBuiltinsTwiceThrows();
    return 0;
}
